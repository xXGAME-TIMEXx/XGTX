#include "xgtx_exchange/bootstrap_budget.hpp"
namespace tools {
wallet2::pending_tx wallet2::create_exchange_bootstrap(xgtx_exchange::operation& op,fee_priority priority,std::uint64_t max_fee,std::uint32_t deadline_window){
  using namespace xgtx_exchange;
  if(m_offline || m_watch_only || m_multisig || key_on_device())throw std::invalid_argument("Bootstrap requires a connected full software wallet");
  if(op.type!=kind::native_deposit || op.limit!=16 || !op.quantity || !max_fee || !deadline_window || deadline_window>1000)throw std::invalid_argument("Invalid bootstrap budget, fee or deadline");
  if(!use_fork_rules(HF_VERSION_VIEW_TAGS,0))throw std::invalid_argument("Bootstrap requires modern tagged RingCT");
  const auto budget=op.quantity;const auto& keys=m_account.get_keys();
  cryptonote::COMMAND_RPC_XGTX_GET_EXCHANGE::request req{};req.limit=1;req.owner=epee::string_tools::pod_to_hex(keys.m_account_address.m_spend_public_key);
  cryptonote::COMMAND_RPC_XGTX_GET_EXCHANGE::response view{};
  {
    const boost::lock_guard<boost::recursive_mutex> lock{m_daemon_rpc_mutex};
    if(!net_utils::invoke_http_json_rpc("/json_rpc","xgtx_get_exchange",req,view,*m_http_client,rpc_timeout) || view.status!=CORE_RPC_STATUS_OK)throw std::runtime_error("Cannot read bootstrap state");
  }
  if(!view.native_backing || wallet_xgtx_decimal(view.reserve_outputs,true)!=0 || view.chain!=epee::string_tools::pod_to_hex(m_blockchain.genesis()) || !epee::string_tools::hex_to_pod(view.chain,op.chain))throw std::invalid_argument("Bootstrap requires an empty reserve on the wallet chain");
  if(view.height<view.activation_height || view.height>std::numeric_limits<std::uint64_t>::max()-deadline_window)throw std::invalid_argument("Bootstrap inactive or deadline overflow");
  op.nonce=wallet_xgtx_decimal(view.next_nonce);op.deadline=view.height+deadline_window;
  // Standard wallet selection authenticates spend keys, fetches ordinary native
  // decoys and enforces maturity. This self-payment is NEVER relayed. Reuse its
  // exact selected inputs to construct the bounded, reviewed all-reserve spend.
  auto candidates=create_transactions_2({{1,keys.m_account_address,false}},adjust_mixin(0),adjust_priority(priority),{},0,{},{});
  if(candidates.size()!=1 || candidates.front().selected_transfers.empty())throw std::invalid_argument("Bootstrap input selection failed");
  auto pending=candidates.front();amount total=0;std::set<std::size_t> unique;
  for(auto index:pending.selected_transfers){
    if(index>=m_transfers.size() || !unique.insert(index).second)throw std::invalid_argument("Invalid selected bootstrap input");
    const auto value=m_transfers[index].amount();if(value>std::numeric_limits<amount>::max()-total)throw std::invalid_argument("Bootstrap input overflow");total+=value;
  }
  const auto base_fee=get_base_fee(adjust_priority(priority));const auto quantization=get_fee_quantization_mask();
  const bool per_byte=use_fork_rules(HF_VERSION_PER_BYTE_FEE,0);amount fee=std::max<amount>(1,pending.fee);
  for(unsigned attempt=0;attempt<8;++attempt){
    op.quantity=bootstrap_credit(total,budget,fee,max_fee);
    const auto intent=cryptonote_sign(op,keys.m_account_address.m_spend_public_key,keys.m_spend_secret_key);op=intent.op;
    std::vector<cryptonote::tx_destination_entry> destinations;
    for(unsigned i=0;i<16;++i){auto address=keys.m_account_address;address.m_spend_public_key=reserve_proofs::derive({op.chain,op.sender,op.nonce},i).public_key;destinations.push_back({deposit_part(op.quantity,16,i),address,false});}
    auto sources=pending.construction_data.sources;cryptonote::transaction tx{};crypto::secret_key txkey{};std::vector<crypto::secret_key> additional;
    const rct::RCTConfig rct_config{rct::RangeProofPaddedBulletproof,4};
    if(!cryptonote::construct_exchange_tx_and_get_tx_key(keys,m_subaddresses,sources,destinations,boost::none,{},tx,txkey,additional,true,rct_config,true,intent))throw std::runtime_error("Bootstrap native transaction construction failed");
    if(tx.vin.size()!=pending.tx.vin.size())throw std::runtime_error("Bootstrap input count changed");
    for(std::size_t i=0;i<tx.vin.size();++i){
      if(tx.vin[i].type()!=typeid(cryptonote::txin_to_key) || pending.tx.vin[i].type()!=typeid(cryptonote::txin_to_key) || boost::get<cryptonote::txin_to_key>(tx.vin[i]).k_image!=boost::get<cryptonote::txin_to_key>(pending.tx.vin[i]).k_image)throw std::runtime_error("Bootstrap selected input order changed");
    }
    const auto blob=cryptonote::tx_to_blob(tx);const auto required=calculate_fee(per_byte,tx,blob.size(),base_fee,quantization);
    if(required>fee){fee=required;continue;}
    pending.tx=std::move(tx);pending.tx_key=txkey;pending.additional_tx_keys=std::move(additional);pending.fee=fee;pending.dust=0;pending.dust_added_to_fee=false;pending.change_dts={};pending.dests=destinations;
    pending.construction_data.sources=std::move(sources);pending.construction_data.change_dts={};pending.construction_data.splitted_dsts=destinations;pending.construction_data.dests=destinations;
    pending.construction_data.extra=pending.tx.extra;pending.construction_data.unlock_time=0;pending.construction_data.use_rct=true;pending.construction_data.use_view_tags=true;pending.construction_data.rct_config=rct_config;
    if(!sanity_check_exchange_deposit({pending},intent))throw std::runtime_error("Bootstrap native input/reserve/fee conservation sanity check failed");
    return pending;
  }
  throw std::runtime_error("Bootstrap fee estimation did not converge");
}
}
