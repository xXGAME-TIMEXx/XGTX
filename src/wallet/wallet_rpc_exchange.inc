#include "xgtx_exchange/native_carrier.hpp"
namespace tools {
namespace {
std::uint64_t rpc_xgtx_decimal(const std::string& text,bool zero_allowed=false){
  if(text.empty() || text.size()>20 || (text.size()>1 && text[0]=='0'))throw std::invalid_argument("Expected canonical decimal amount");
  std::uint64_t value=0;
  for(char digit:text){if(digit<'0' || digit>'9' || value>(std::numeric_limits<std::uint64_t>::max()-(digit-'0'))/10)throw std::invalid_argument("Amount out of range");value=value*10+(digit-'0');}
  if(!zero_allowed && !value)throw std::invalid_argument("Amount must be positive");return value;
}
xgtx_exchange::id rpc_xgtx_id(const std::string& text){xgtx_exchange::id value{};if(text.empty())return value;if(text.size()!=64 || !epee::string_tools::hex_to_pod(text,value))throw std::invalid_argument("Expected 32-byte asset ID");return value;}
}
bool wallet_rpc_server::on_xgtx_identity(const wallet_rpc::COMMAND_RPC_XGTX_IDENTITY::request&,wallet_rpc::COMMAND_RPC_XGTX_IDENTITY::response& res,epee::json_rpc::error& er,const connection_context*){
  if(m_restricted){er.code=WALLET_RPC_ERROR_CODE_DENIED;er.message="Command unavailable in restricted mode";return false;}
  if(!m_wallet)return not_open(er);
  const auto& address=m_wallet->get_account().get_keys().m_account_address;
  res.address=cryptonote::get_account_address_as_str(m_wallet->nettype(),false,address);
  res.owner=epee::string_tools::pod_to_hex(address.m_spend_public_key);res.chain=epee::string_tools::pod_to_hex(m_wallet->exchange_genesis());return true;
}
bool wallet_rpc_server::on_xgtx_relay_operation(const wallet_rpc::COMMAND_RPC_XGTX_RELAY_OPERATION::request& req,wallet_rpc::COMMAND_RPC_XGTX_RELAY_OPERATION::response& res,epee::json_rpc::error& er,const connection_context*){
  if(m_restricted){er.code=WALLET_RPC_ERROR_CODE_DENIED;er.message="Command unavailable in restricted mode";return false;}
  if(!m_wallet)return not_open(er);
  try{
    crypto::hash draft{};if(req.draft_id.size()!=64 || !epee::string_tools::hex_to_pod(req.draft_id,draft))throw std::invalid_argument("Invalid draft identifier");
    const auto pending=m_wallet->relay_exchange_draft(draft);const auto blob=cryptonote::tx_to_blob(pending.tx);
    const auto genesis=m_wallet->exchange_genesis();xgtx_exchange::id chain{};std::memcpy(chain.data(),&genesis,32);
    const auto intent=xgtx_exchange::verify_native_carrier(pending.tx,chain);
    res.tx_hash=epee::string_tools::pod_to_hex(cryptonote::get_transaction_hash(pending.tx));res.tx_blob=epee::string_tools::buff_to_hex_nodelimer(blob);
    res.fee=std::to_string(pending.fee);res.weight=std::to_string(cryptonote::get_transaction_weight(pending.tx,blob.size()));res.relayed=true;res.draft_id=req.draft_id;
    res.chain=epee::string_tools::pod_to_hex(intent.op.chain);res.owner=epee::string_tools::pod_to_hex(intent.op.sender);res.nonce=std::to_string(intent.op.nonce);res.deadline=std::to_string(intent.op.deadline);
    if(intent.op.type==xgtx_exchange::kind::native_deposit)res.deposit_quantity=std::to_string(intent.op.quantity);
    if(intent.op.type==xgtx_exchange::kind::native_withdraw)res.payout=std::to_string(intent.op.quantity-pending.fee);
    return true;
  }catch(const std::exception&){handle_rpc_exception(std::current_exception(),er,WALLET_RPC_ERROR_CODE_GENERIC_TRANSFER_ERROR);return false;}
}

bool wallet_rpc_server::on_xgtx_operation(const wallet_rpc::COMMAND_RPC_XGTX_OPERATION::request& req,wallet_rpc::COMMAND_RPC_XGTX_OPERATION::response& res,epee::json_rpc::error& er,const connection_context*){
  if(m_restricted){er.code=WALLET_RPC_ERROR_CODE_DENIED;er.message="Command unavailable in restricted mode";return false;}
  if(!m_wallet)return not_open(er);
  try{
    if(req.kind<1 || req.kind>9 || req.decimals>12 || !fee_priority_utilities::is_valid(req.priority))throw std::invalid_argument("Invalid operation kind, decimals or priority");
    xgtx_exchange::operation op;op.type=static_cast<xgtx_exchange::kind>(req.kind);op.asset=rpc_xgtx_id(req.asset);op.other=rpc_xgtx_id(req.other);
    op.quantity=rpc_xgtx_decimal(req.quantity);op.limit=rpc_xgtx_decimal(req.limit,true);op.minimum=rpc_xgtx_decimal(req.minimum,true);op.ticker=req.ticker;op.decimals=req.decimals;
    cryptonote::address_parse_info recipient{};
    if(op.type==xgtx_exchange::kind::transfer || op.type==xgtx_exchange::kind::native_withdraw){
      if(!cryptonote::get_account_address_from_str(recipient,m_wallet->nettype(),req.recipient) || recipient.is_subaddress || recipient.has_payment_id)throw std::invalid_argument("Token recipient must be a standard wallet address");
      if(op.type==xgtx_exchange::kind::transfer)std::memcpy(op.recipient.data(),&recipient.address.m_spend_public_key,32);
    }else if(!req.recipient.empty())throw std::invalid_argument("Recipient is only used for transfers and withdrawals");
    std::vector<wallet2::pending_tx> transactions;
    if(req.bootstrap){
      if(op.type!=xgtx_exchange::kind::native_deposit || !req.do_not_relay || op.asset!=xgtx_exchange::id{} || op.other!=xgtx_exchange::id{} || op.recipient!=xgtx_exchange::id{} || op.limit!=16 || op.minimum || op.decimals || !op.ticker.empty())throw std::invalid_argument("Bootstrap requires deposit kind, limit16, maximum input budget and preview before relay");
      transactions.push_back(m_wallet->create_exchange_bootstrap(op,fee_priority_utilities::from_integral(req.priority),rpc_xgtx_decimal(req.max_fee),req.deadline_window));
    }else if(op.type==xgtx_exchange::kind::native_withdraw){
      if(op.asset!=xgtx_exchange::id{} || op.other!=xgtx_exchange::id{} || op.limit || op.minimum || op.decimals || !op.ticker.empty())throw std::invalid_argument("Unexpected withdrawal fields");
      transactions.push_back(m_wallet->create_exchange_withdrawal(op,recipient.address,fee_priority_utilities::from_integral(req.priority),rpc_xgtx_decimal(req.max_fee),req.deadline_window));
    }else transactions=m_wallet->create_exchange_transactions(op,fee_priority_utilities::from_integral(req.priority),rpc_xgtx_decimal(req.max_fee),req.deadline_window);
    auto intent=xgtx_exchange::verify_native_carrier(transactions.front().tx,op.chain);
    if(op.type==xgtx_exchange::kind::native_deposit)res.deposit_quantity=std::to_string(intent.op.quantity);
    auto encoded=xgtx_exchange::encode_envelope(intent);res.signed_intent=epee::string_tools::buff_to_hex_nodelimer(std::string(reinterpret_cast<const char*>(encoded.data()),encoded.size()));
    res.chain=epee::string_tools::pod_to_hex(op.chain);res.owner=epee::string_tools::pod_to_hex(op.sender);res.nonce=std::to_string(op.nonce);res.deadline=std::to_string(op.deadline);
    if(req.do_not_relay)res.draft_id=epee::string_tools::pod_to_hex(m_wallet->retain_exchange_draft(transactions.front()));
    if(op.type==xgtx_exchange::kind::create){xgtx_exchange::engine engine({op.chain,op.sender,0,1024},xgtx_exchange::cryptonote_hash,xgtx_exchange::cryptonote_verify);res.asset_id=epee::string_tools::pod_to_hex(engine.asset_id(op));}
    if(op.type==xgtx_exchange::kind::native_withdraw){
      const auto& pending=transactions.front();const auto blob=cryptonote::tx_to_blob(pending.tx);
      res.tx_hash=epee::string_tools::pod_to_hex(cryptonote::get_transaction_hash(pending.tx));res.tx_blob=epee::string_tools::buff_to_hex_nodelimer(blob);
      res.fee=std::to_string(pending.fee);res.weight=std::to_string(cryptonote::get_transaction_weight(pending.tx,blob.size()));res.payout=std::to_string(op.quantity-pending.fee);
      if(!req.do_not_relay)m_wallet->relay_exchange_withdrawal(pending);
      res.relayed=!req.do_not_relay;return true;
    }
    wallet_rpc::COMMAND_RPC_TRANSFER::response transfer{};
    if(!fill_response(transactions,false,transfer.tx_key,transfer.amount,transfer.amounts_by_dest,transfer.fee,transfer.weight,transfer.multisig_txset,transfer.unsigned_txset,req.do_not_relay,transfer.tx_hash,true,transfer.tx_blob,false,transfer.tx_metadata,transfer.spent_key_images,er))return false;
    res.tx_hash=transfer.tx_hash;res.tx_blob=transfer.tx_blob;res.fee=std::to_string(transfer.fee);res.weight=std::to_string(transfer.weight);res.relayed=!req.do_not_relay;return true;
  }catch(const std::exception&){handle_rpc_exception(std::current_exception(),er,WALLET_RPC_ERROR_CODE_GENERIC_TRANSFER_ERROR);return false;}
}
}
