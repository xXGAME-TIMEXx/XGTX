#include "xgtx_exchange/native_withdraw.hpp"
namespace tools {
bool wallet2::sanity_check_exchange_deposit(const std::vector<pending_tx>& transactions,const xgtx_exchange::envelope& intent) const {
  if(transactions.size()!=1 || intent.op.type!=xgtx_exchange::kind::native_deposit || (intent.op.limit!=1 && intent.op.limit!=16))return false;
  const auto& ptx=transactions.front();
  if(ptx.tx.vout.size()!=(intent.op.limit==16?16:2) || ptx.tx.rct_signatures.txnFee!=ptx.fee)return false;
  const auto actual=xgtx_exchange::verify_native_carrier(ptx.tx,intent.op.chain);
  if(xgtx_exchange::encode_envelope(actual)!=xgtx_exchange::encode_envelope(intent))return false;
  const auto mask=xgtx_exchange::deposit_output_mask(ptx.tx,intent.op.chain);
  if(intent.op.limit==16){if(mask.size()!=16 || std::any_of(mask.begin(),mask.end(),[](std::uint8_t n){return n!=1;}))return false;}
  else if(mask.size()!=2 || mask[0]!=1 || mask[1]!=0)return false;
  if(!rct::verRctSimple(ptx.tx.rct_signatures))return false;
  std::uint64_t inputs=0;
  std::set<std::size_t> selected;
  for(auto index:ptx.selected_transfers){
    if(index>=m_transfers.size() || !selected.insert(index).second)return false;
    const auto amount=m_transfers[index].amount();
    if(inputs>std::numeric_limits<std::uint64_t>::max()-amount)return false;
    inputs+=amount;
  }
  if(inputs<ptx.fee || inputs-ptx.fee<intent.op.quantity)return false;
  const auto change=inputs-ptx.fee-intent.op.quantity;
  if(change!=ptx.change_dts.amount || (intent.op.limit==16 && change))return false;
  if(change){
    if(m_subaddresses.find(ptx.change_dts.addr.m_spend_public_key)==m_subaddresses.end())return false;
    std::uint64_t received=0;
    auto proof=get_tx_proof(ptx.tx,ptx.tx_key,ptx.additional_tx_keys,ptx.change_dts.addr,ptx.change_dts.is_subaddress,"deposit-change-sanity-check");
    check_tx_proof(ptx.tx,ptx.change_dts.addr,ptx.change_dts.is_subaddress,"deposit-change-sanity-check",proof,received);
    if(received!=change)return false;
  }
  return !selected.empty();
}
namespace {
std::uint64_t wallet_xgtx_decimal(const std::string& text,bool zero_allowed=false){
  if(text.empty() || text.size()>20 || (text.size()>1 && text[0]=='0'))throw std::invalid_argument("Expected canonical decimal amount");
  std::uint64_t value=0;
  for(char digit:text){if(digit<'0' || digit>'9' || value>(std::numeric_limits<std::uint64_t>::max()-(digit-'0'))/10)throw std::invalid_argument("Amount out of range");value=value*10+(digit-'0');}
  if(!zero_allowed && !value)throw std::invalid_argument("Amount must be positive");return value;
}
}
std::vector<wallet2::pending_tx> wallet2::create_exchange_transactions(xgtx_exchange::operation& op,fee_priority priority,std::uint64_t max_fee,std::uint32_t deadline_window){
  if(m_offline || m_watch_only || m_multisig || key_on_device())throw std::invalid_argument("Exchange requires a connected full software wallet");
  if(static_cast<unsigned>(op.type)<1 || static_cast<unsigned>(op.type)>8)throw std::invalid_argument("Use a supported token, transfer, liquidity or swap operation");
  if(!max_fee || !deadline_window || deadline_window>1000)throw std::invalid_argument("Invalid maximum fee or deadline window");
  if(!use_fork_rules(HF_VERSION_CLSAG,0))throw std::invalid_argument("Exchange requires modern native RingCT");
  const auto& keys=m_account.get_keys();
  cryptonote::COMMAND_RPC_XGTX_GET_EXCHANGE::request req{};req.limit=1;req.owner=epee::string_tools::pod_to_hex(keys.m_account_address.m_spend_public_key);
  cryptonote::COMMAND_RPC_XGTX_GET_EXCHANGE::response view{};
  {
    const boost::lock_guard<boost::recursive_mutex> lock{m_daemon_rpc_mutex};
    if(!net_utils::invoke_http_json_rpc("/json_rpc","xgtx_get_exchange",req,view,*m_http_client,rpc_timeout) || view.status!=CORE_RPC_STATUS_OK)throw std::runtime_error("Cannot read configured exchange state from daemon");
  }
  if(view.chain!=epee::string_tools::pod_to_hex(m_blockchain.genesis()))throw std::invalid_argument("Exchange chain does not match wallet genesis");
  if(view.height<view.activation_height || view.height>std::numeric_limits<std::uint64_t>::max()-deadline_window)throw std::invalid_argument("Exchange not active or deadline overflow");
  if(!epee::string_tools::hex_to_pod(view.chain,op.chain))throw std::invalid_argument("Invalid exchange chain ID");
  if(op.type==xgtx_exchange::kind::native_deposit && (!view.native_backing || op.limit!=1 || wallet_xgtx_decimal(view.reserve_outputs,true)<16))
    throw std::invalid_argument("Wallet deposit requires native backing, one reserve output and an existing sixteen-output bootstrap");
  op.nonce=wallet_xgtx_decimal(view.next_nonce);op.deadline=view.height+deadline_window;
  auto intent=xgtx_exchange::cryptonote_sign(op,keys.m_account_address.m_spend_public_key,keys.m_spend_secret_key);op=intent.op;
  // Small ordinary self-payment supplies standard native input selection, fee
  // calculation and change handling. All DEX value is in the signed operation.
  std::vector<cryptonote::tx_destination_entry> destinations{{1,keys.m_account_address,false}};
  if(op.type==xgtx_exchange::kind::native_deposit){
    auto reserve_address=keys.m_account_address;
    reserve_address.m_spend_public_key=xgtx_exchange::reserve_proofs::derive({op.chain,op.sender,op.nonce},0).public_key;
    destinations={{op.quantity,reserve_address,false}};
  }
  auto result=create_transactions_2(destinations,adjust_mixin(0),adjust_priority(priority),{},0,{}, {},&intent);
  if(result.size()!=1)throw std::invalid_argument("Exchange operation cannot be split across native transactions");
  if(result.front().fee>max_fee)throw std::invalid_argument("Estimated native transaction fee exceeds maximum fee");
  xgtx_exchange::verify_native_carrier(result.front().tx,op.chain);
  return result;
}
// Native reserve inputs are segregated ledger backing, not ordinary wallet
// transfers. Never pass this pending transaction to ordinary commit_tx bookkeeping.
wallet2::pending_tx wallet2::create_exchange_withdrawal(xgtx_exchange::operation& op,const cryptonote::account_public_address& recipient,fee_priority priority,std::uint64_t max_fee,std::uint32_t deadline_window){
  using namespace xgtx_exchange;
  if(m_offline || m_watch_only || m_multisig || key_on_device())throw std::invalid_argument("Withdrawal requires a connected full software wallet");
  if(!max_fee || !deadline_window || deadline_window>1000 || !op.quantity || op.type!=kind::native_withdraw)throw std::invalid_argument("Invalid withdrawal amount, fee or deadline");
  if(!use_fork_rules(HF_VERSION_VIEW_TAGS,0))throw std::invalid_argument("Withdrawal requires tagged native RingCT outputs");
  if(!crypto::check_key(recipient.m_spend_public_key) || !crypto::check_key(recipient.m_view_public_key))throw std::invalid_argument("Invalid withdrawal recipient");
  const auto& keys=m_account.get_keys();id owner{};std::memcpy(owner.data(),&keys.m_account_address.m_spend_public_key,32);
  cryptonote::COMMAND_RPC_XGTX_GET_EXCHANGE::request req{};req.limit=1;req.owner=epee::string_tools::pod_to_hex(owner);
  cryptonote::COMMAND_RPC_XGTX_GET_EXCHANGE::response view{};
  {
    const boost::lock_guard<boost::recursive_mutex> lock{m_daemon_rpc_mutex};
    if(!net_utils::invoke_http_json_rpc("/json_rpc","xgtx_get_exchange",req,view,*m_http_client,rpc_timeout) || view.status!=CORE_RPC_STATUS_OK)throw std::runtime_error("Cannot read native ledger");
  }
  if(!view.native_backing || view.chain!=epee::string_tools::pod_to_hex(m_blockchain.genesis()) || !epee::string_tools::hex_to_pod(view.chain,op.chain))throw std::invalid_argument("Backing chain differs from wallet genesis");
  if(view.height<view.activation_height || view.height>std::numeric_limits<std::uint64_t>::max()-deadline_window)throw std::invalid_argument("Withdrawal not active or deadline overflow");
  if(view.assets.empty() || view.assets.front().id!=std::string(64,'0') || wallet_xgtx_decimal(view.assets.front().balance,true)<op.quantity)throw std::invalid_argument("Insufficient confirmed native ledger balance");
  op.nonce=wallet_xgtx_decimal(view.next_nonce);op.deadline=view.height+deadline_window;
  const auto count=wallet_xgtx_decimal(view.reserve_outputs,true);if(count<16)throw std::invalid_argument("Reserve bootstrap missing");
  struct input_source {cryptonote::txin_to_key input{};rct::ctkey secret{};rct::ctkeyV ring;amount value=0;unsigned real=0;};
  std::vector<input_source> selected;amount total=0;std::set<std::string> images;std::map<std::string,bool> spent_status;std::map<amount,native_reserve_source> authenticated;
  // Bounded confirmed lookup; all requests bind the same tip. Current policy
  // conservatively requires zero unlock time and ten blocks for every member.
  for(amount start=0,pages=0;total<op.quantity && pages<256;++pages){
    cryptonote::COMMAND_RPC_XGTX_GET_RESERVE_RING::request ring_req{};ring_req.start=std::to_string(start);ring_req.tip=view.tip;
    cryptonote::COMMAND_RPC_XGTX_GET_RESERVE_RING::response ring{};
    {
      const boost::lock_guard<boost::recursive_mutex> lock{m_daemon_rpc_mutex};
      if(!net_utils::invoke_http_json_rpc("/json_rpc","xgtx_get_reserve_ring",ring_req,ring,*m_http_client,rpc_timeout) || ring.status!=CORE_RPC_STATUS_OK)throw std::runtime_error("Cannot read stable reserve ring");
    }
    if(ring.chain!=view.chain || ring.tip!=view.tip || ring.height!=view.height || wallet_xgtx_decimal(ring.count)!=count || ring.outputs.size()!=16)throw std::runtime_error("Reserve response differs from confirmed snapshot");
    rct::ctkeyV members;std::vector<reserve_proofs::verified_output> verified;std::vector<native_reserve_source> sources;bool mature=true;
    for(unsigned j=0;j<16;++j){
      const auto& row=ring.outputs[j];
      if(wallet_xgtx_decimal(row.global_index,true)!=start+j || row.origin.size()>256*1024)throw std::runtime_error("Invalid reserve ring index or origin size");
      native_reserve_source source;std::string blob;
      if(!epee::string_tools::parse_hexstr_to_binbuff(row.origin,blob) || !cryptonote::parse_and_validate_tx_base_from_blob(blob,source.origin) ||
         !epee::string_tools::hex_to_pod(row.transaction,source.transaction) || !epee::string_tools::hex_to_pod(row.prunable_hash,source.prunable_hash) ||
         !epee::string_tools::hex_to_pod(row.key,source.stored.dest) || !epee::string_tools::hex_to_pod(row.commitment,source.stored.mask))throw std::runtime_error("Malformed reserve origin proof");
      source.index=row.output_index;const auto proof=verify_reserve_source(source,op.chain);
      const auto status=spent_status.emplace(epee::string_tools::pod_to_hex(proof.image),row.spent);
      if(!status.second && status.first->second!=row.spent)throw std::runtime_error("Inconsistent reserve spent status");
      if(row.unlock_time || source.origin.unlock_time || row.height>=view.height || view.height-row.height<10)mature=false;
      members.push_back(source.stored);verified.push_back(proof);sources.push_back(std::move(source));
    }
    if(mature)for(unsigned j=0;j<16 && total<op.quantity;++j){
      const auto image=epee::string_tools::pod_to_hex(verified[j].image);
      if(ring.outputs[j].spent || !images.insert(image).second)continue;
      if(selected.size()==16)throw std::invalid_argument("Withdrawal requires more than sixteen reserve inputs; use a smaller amount");
      if(verified[j].quantity>std::numeric_limits<amount>::max()-total)throw std::invalid_argument("Reserve amount overflow");
      const auto origin_intent=verify_native_carrier(sources[j].origin,op.chain);
      const auto opening=reserve_proofs::derive({op.chain,origin_intent.op.sender,origin_intent.op.nonce},sources[j].index);
      input_source input;input.input.amount=reserve_policy::bucket;input.input.k_image=verified[j].image;
      input.input.key_offsets.assign(16,1);input.input.key_offsets[0]=start;input.real=j;input.value=verified[j].quantity;
      input.secret={opening.spend,verified[j].mask};input.ring=members;selected.push_back(std::move(input));total+=verified[j].quantity;
      for(unsigned k=0;k<16;++k)authenticated.emplace(start+k,sources[k]);
    }
    if(start==count-16)break;
    start=count-16-start<16?count-16:start+16;
  }
  if(total<op.quantity)throw std::invalid_argument("Insufficient mature reserve liquidity within bounded lookup; retry later or use a smaller amount");
  std::sort(selected.begin(),selected.end(),[](const input_source& a,const input_source& b){return std::memcmp(&a.input.k_image,&b.input.k_image,32)>0;});
  op.limit=total-op.quantity;
  const auto base_fee=get_base_fee(adjust_priority(priority));const auto quantization=get_fee_quantization_mask();
  const bool per_byte=use_fork_rules(HF_VERSION_PER_BYTE_FEE,0);amount fee=1;
  for(unsigned attempt=0;attempt<8;++attempt){
    if(fee>max_fee || fee>=op.quantity)throw std::invalid_argument("Withdrawal fee exceeds ceiling or leaves no payout");
    cryptonote::transaction original;original.version=2;rct::ctkeyV secrets,out_secrets;rct::ctkeyM mix;std::vector<unsigned> real;
    std::vector<rct::xmr_amount> amounts_in,amounts_out;rct::keyV destinations,amount_keys;
    for(const auto& source:selected){original.vin.push_back(source.input);secrets.push_back(source.secret);mix.push_back(source.ring);real.push_back(source.real);amounts_in.push_back(source.value);}
    crypto::secret_key txsecret{};crypto::public_key txpublic{};crypto::generate_keys(txpublic,txsecret);cryptonote::add_tx_pub_key_to_extra(original,txpublic);
    const auto payout=op.quantity-fee;
    for(unsigned i=0;i<2;++i){
      crypto::public_key pub{};crypto::view_tag tag{};rct::key amount_key{};amount value=0;
      if(op.limit && i==0){const auto reserve=reserve_proofs::derive({op.chain,owner,op.nonce},0);pub=reserve.public_key;amount_key=reserve.amount_key;value=op.limit;}
      else {
        crypto::key_derivation derivation{};crypto::secret_key scalar{};
        if(!crypto::generate_key_derivation(recipient.m_view_public_key,txsecret,derivation) || !crypto::derive_public_key(derivation,i,recipient.m_spend_public_key,pub))throw std::runtime_error("Payout key derivation failed");
        crypto::derivation_to_scalar(derivation,i,scalar);crypto::derive_view_tag(derivation,i,tag);amount_key=rct::sk2rct(scalar);
        value=op.limit?payout:(i==0?payout/2:payout-payout/2);
      }
      cryptonote::tx_out output{};cryptonote::set_tx_out(0,pub,true,tag,output);original.vout.push_back(output);
      destinations.push_back(rct::pk2rct(pub));amount_keys.push_back(amount_key);amounts_out.push_back(value);
    }
    op.recipient=withdrawal_payout_binding(original,op.chain,op.limit!=0);
    const auto intent=cryptonote_sign(op,keys.m_account_address.m_spend_public_key,keys.m_spend_secret_key);op=intent.op;
    const auto prefix=cryptonote::get_transaction_prefix_hash(original);auto& device=hw::get_device("default");
    auto tx=sign_carrier_transaction(original,intent,keys.m_account_address.m_spend_public_key,keys.m_spend_secret_key,
      [&](const std::function<rct::key(const rct::rctSig&)>& finalize){return rct::genRctSimpleWithMessageFinalizer(rct::hash2rct(prefix),secrets,destinations,amounts_in,amounts_out,fee,mix,amount_keys,real,out_secrets,{rct::RangeProofPaddedBulletproof,4},device,finalize);});
    const auto blob=cryptonote::tx_to_blob(tx);const auto required=calculate_fee(per_byte,tx,blob.size(),base_fee,quantization);
    if(required>fee){fee=required;continue;}
    // Verify the complete reserve effect from independently authenticated
    // origins before returning a draft. Native daemon still verifies inclusion,
    // maturity, spentness, ledger balance and ordinary RingCT consensus.
    xgtx_exchange::config cfg{op.chain,owner,0,1024};cfg.native_backing=true;xgtx_exchange::engine adapter(cfg,cryptonote_hash,cryptonote_verify);
    withdrawal_native_effect(tx,adapter,[&](amount index){return authenticated.at(index);},[&](const crypto::key_image& image){return spent_status.at(epee::string_tools::pod_to_hex(image));});
    pending_tx result{};result.tx=std::move(tx);result.tx_key=txsecret;result.fee=fee;return result;
  }
  throw std::runtime_error("Withdrawal fee estimation did not converge");
}
void wallet2::relay_exchange_withdrawal(const pending_tx& ptx){
  xgtx_exchange::id chain{};const auto genesis=m_blockchain.genesis();std::memcpy(chain.data(),&genesis,32);
  const auto intent=xgtx_exchange::verify_native_carrier(ptx.tx,chain);xgtx_exchange::id owner{};
  std::memcpy(owner.data(),&m_account.get_keys().m_account_address.m_spend_public_key,32);
  if(intent.op.type!=xgtx_exchange::kind::native_withdraw || intent.op.sender!=owner || !ptx.selected_transfers.empty() || ptx.fee!=ptx.tx.rct_signatures.txnFee)throw std::invalid_argument("Invalid segregated withdrawal transaction");
  cryptonote::COMMAND_RPC_SEND_RAW_TX::request req{};req.tx_as_hex=epee::string_tools::buff_to_hex_nodelimer(cryptonote::tx_to_blob(ptx.tx));req.do_not_relay=false;req.do_sanity_checks=true;
  cryptonote::COMMAND_RPC_SEND_RAW_TX::response response{};
  {
    const boost::lock_guard<boost::recursive_mutex> lock{m_daemon_rpc_mutex};
    const bool ok=net_utils::invoke_http_json("/sendrawtransaction",req,response,*m_http_client,rpc_timeout);
    THROW_ON_RPC_RESPONSE_ERROR(ok,{},response,"sendrawtransaction",error::tx_rejected,ptx.tx,get_rpc_status(m_trusted_daemon,response.status),get_text_reason(response));
  }
  // No ordinary wallet inputs were spent. Store only the outgoing proof key;
  // scanning the recipient's standard outputs uses the normal wallet path.
  if(m_store_tx_info)m_tx_keys[cryptonote::get_transaction_hash(ptx.tx)]=ptx.tx_key;
}

crypto::hash wallet2::exchange_genesis() const {
  cryptonote::block genesis;generate_genesis(genesis);return cryptonote::get_block_hash(genesis);
}
crypto::hash wallet2::retain_exchange_draft(const pending_tx& transaction){
  const boost::lock_guard<boost::recursive_mutex> lock{m_exchange_draft_mutex};
  const auto now=std::chrono::steady_clock::now();
  for(auto it=m_exchange_drafts.begin();it!=m_exchange_drafts.end();)if(it->second.first<=now)it=m_exchange_drafts.erase(it);else ++it;
  const auto genesis=exchange_genesis();xgtx_exchange::id chain{},owner{};
  std::memcpy(chain.data(),&genesis,32);std::memcpy(owner.data(),&m_account.get_keys().m_account_address.m_spend_public_key,32);
  const auto intent=xgtx_exchange::verify_native_carrier(transaction.tx,chain);
  if(intent.op.sender!=owner)throw std::invalid_argument("Draft owner differs from open wallet");
  if(m_exchange_drafts.size()>=8){
    auto oldest=std::min_element(m_exchange_drafts.begin(),m_exchange_drafts.end(),[](const auto& a,const auto& b){return a.second.first<b.second.first;});
    m_exchange_drafts.erase(oldest);
  }
  crypto::hash draft{};do{draft=crypto::rand<crypto::hash>();}while(m_exchange_drafts.count(draft));
  m_exchange_drafts.emplace(draft,std::make_pair(now+std::chrono::minutes(10),transaction));return draft;
}
wallet2::pending_tx wallet2::relay_exchange_draft(const crypto::hash& draft){
  const boost::lock_guard<boost::recursive_mutex> lock{m_exchange_draft_mutex};
  auto it=m_exchange_drafts.find(draft);
  if(it==m_exchange_drafts.end())throw std::invalid_argument("Draft missing, already submitted or wallet restarted; preview again");
  if(it->second.first<=std::chrono::steady_clock::now()){m_exchange_drafts.erase(it);throw std::invalid_argument("Draft expired; preview again");}
  auto pending=it->second.second;
  const auto genesis=exchange_genesis();xgtx_exchange::id chain{},owner{};
  std::memcpy(chain.data(),&genesis,32);std::memcpy(owner.data(),&m_account.get_keys().m_account_address.m_spend_public_key,32);
  const auto intent=xgtx_exchange::verify_native_carrier(pending.tx,chain);
  if(intent.op.sender!=owner)throw std::invalid_argument("Draft owner differs from open wallet");
  cryptonote::COMMAND_RPC_XGTX_GET_EXCHANGE::request req{};req.owner=epee::string_tools::pod_to_hex(owner);req.limit=1;
  cryptonote::COMMAND_RPC_XGTX_GET_EXCHANGE::response view{};
  {
    const boost::lock_guard<boost::recursive_mutex> daemon_lock{m_daemon_rpc_mutex};
    if(!net_utils::invoke_http_json_rpc("/json_rpc","xgtx_get_exchange",req,view,*m_http_client,rpc_timeout) || view.status!=CORE_RPC_STATUS_OK)throw std::runtime_error("Cannot verify draft nonce and deadline");
  }
  if(view.chain!=epee::string_tools::pod_to_hex(chain) || wallet_xgtx_decimal(view.next_nonce)!=intent.op.nonce || view.height>intent.op.deadline)
    throw std::invalid_argument("Draft nonce, chain or deadline changed; preview again");
  if(intent.op.type==xgtx_exchange::kind::native_withdraw)relay_exchange_withdrawal(pending);else commit_tx(pending);
  m_exchange_drafts.erase(draft);return pending;
}

}
