#include <cstring>
#include "xgtx_exchange/native_withdraw.hpp"
// Included at EOF of core_rpc_server.cpp, after native types and RPC macros.
namespace cryptonote {
namespace {
xgtx_exchange::id xgtx_rpc_id(const std::string& text){
  xgtx_exchange::id value{};
  if(text.size()!=64 || !epee::string_tools::hex_to_pod(text,value))throw std::invalid_argument("Expected 32-byte hexadecimal ID");
  return value;
}
std::string xgtx_rpc_hex(const xgtx_exchange::id& value){return epee::string_tools::pod_to_hex(value);}
void xgtx_rpc_check_snapshot(const xgtx_exchange::state& snapshot,const BlockchainDB& db){
  if(snapshot.height!=db.height())throw std::runtime_error("Native/exchange projection mismatch");
  if(snapshot.height){const auto tip=db.top_block_hash();if(std::memcmp(snapshot.tip.data(),&tip,32)!=0)throw std::runtime_error("Native/exchange tip mismatch");}
}
xgtx_exchange::amount xgtx_rpc_amount(const std::string& text){
  if(text.empty() || text.size()>20 || text[0]<'1' || text[0]>'9')throw std::invalid_argument("Expected positive canonical decimal amount");
  xgtx_exchange::amount value=0;
  for(char digit:text){if(digit<'0' || digit>'9' || value>(std::numeric_limits<xgtx_exchange::amount>::max()-(digit-'0'))/10)throw std::invalid_argument("Amount out of range");value=value*10+(digit-'0');}
  return value;
}
}
bool core_rpc_server::on_xgtx_get_reserve_ring(const COMMAND_RPC_XGTX_GET_RESERVE_RING::request& req,COMMAND_RPC_XGTX_GET_RESERVE_RING::response& res,epee::json_rpc::error& error,const connection_context*){
  RPC_TRACKER(xgtx_get_reserve_ring);
  try{
    const auto expected=xgtx_rpc_id(req.tip);
    const auto start=req.start=="0"?0:xgtx_rpc_amount(req.start);
    auto& blockchain=m_core.get_blockchain_storage();CRITICAL_REGION_LOCAL(blockchain);
    const auto& db=blockchain.get_db();const auto* exchange=db.experimental_exchange_engine();
    if(!exchange || !exchange->settings().native_backing){error.code=-32601;error.message="Native backing is not enabled";return false;}
    const auto snapshot=db.experimental_exchange_state();xgtx_rpc_check_snapshot(snapshot,db);
    if(expected!=snapshot.tip)throw std::invalid_argument("Reserve snapshot changed; refresh and retry");
    const auto count=db.get_num_outputs(xgtx_exchange::native_reserve_bucket);
    if(count<16 || start>count-16)throw std::invalid_argument("Reserve ring requires sixteen existing outputs");
    res.chain=xgtx_rpc_hex(exchange->settings().chain);res.tip=xgtx_rpc_hex(snapshot.tip);res.height=snapshot.height;res.count=std::to_string(count);
    for(std::uint64_t i=start;i<start+16;++i){
      const auto location=db.get_output_tx_and_index(xgtx_exchange::native_reserve_bucket,i);
      const auto record=db.get_output_key(xgtx_exchange::native_reserve_bucket,i,true);
      cryptonote::blobdata blob;
      if(!db.get_pruned_tx_blob(location.first,blob) || blob.size()>128*1024)throw std::runtime_error("Reserve origin exceeds bounded wallet response");
      xgtx_exchange::native_reserve_source source;source.origin=db.get_pruned_tx(location.first);
      std::memcpy(source.transaction.data(),&location.first,32);
      crypto::hash prunable{};if(!db.get_prunable_tx_hash(location.first,prunable))throw std::runtime_error("Missing reserve origin hash");
      std::memcpy(source.prunable_hash.data(),&prunable,32);
      if(location.second>std::numeric_limits<std::uint32_t>::max())throw std::runtime_error("Reserve origin index overflow");
      source.index=location.second;source.stored={rct::pk2rct(record.pubkey),record.commitment};
      const auto verified=xgtx_exchange::verify_reserve_source(source,exchange->settings().chain);
      xgtx_rpc_reserve row;row.global_index=std::to_string(i);row.transaction=epee::string_tools::pod_to_hex(location.first);
      row.prunable_hash=epee::string_tools::pod_to_hex(prunable);row.origin=epee::string_tools::buff_to_hex_nodelimer(blob);
      row.key=epee::string_tools::pod_to_hex(record.pubkey);row.commitment=epee::string_tools::pod_to_hex(record.commitment);
      row.output_index=source.index;row.height=record.height;row.unlock_time=record.unlock_time;row.spent=db.has_key_image(verified.image);
      res.outputs.push_back(std::move(row));
    }
    res.status=CORE_RPC_STATUS_OK;return true;
  }catch(const std::invalid_argument& failure){error.code=CORE_RPC_ERROR_CODE_WRONG_PARAM;error.message=failure.what();return false;}
  catch(const std::exception& failure){error.code=CORE_RPC_ERROR_CODE_INTERNAL_ERROR;error.message=failure.what();return false;}
}

bool core_rpc_server::on_xgtx_get_exchange(const COMMAND_RPC_XGTX_GET_EXCHANGE::request& req,COMMAND_RPC_XGTX_GET_EXCHANGE::response& res,epee::json_rpc::error& error,const connection_context*){
  RPC_TRACKER(xgtx_get_exchange);
  try{
    if(req.limit<1 || req.limit>100)throw std::invalid_argument("Page limit must be 1 through 100");
    std::optional<xgtx_exchange::id> owner,asset_cursor,pool_cursor;
    if(!req.owner.empty()){
      owner=xgtx_rpc_id(req.owner);crypto::public_key pub{};std::memcpy(&pub,owner->data(),32);
      if(!crypto::check_key(pub))throw std::invalid_argument("Invalid owner public key");
    }
    if(!req.after_asset.empty())asset_cursor=xgtx_rpc_id(req.after_asset);
    if(!req.after_pool.empty())pool_cursor=xgtx_rpc_id(req.after_pool);
    auto& blockchain=m_core.get_blockchain_storage();CRITICAL_REGION_LOCAL(blockchain);
    const auto& db=blockchain.get_db();const auto* exchange=db.experimental_exchange_engine();
    if(!exchange){error.code=-32601;error.message="Exchange is not enabled on this node";return false;}
    const auto snapshot=db.experimental_exchange_state();
    xgtx_rpc_check_snapshot(snapshot,db);
    const auto& cfg=exchange->settings();
    res.chain=xgtx_rpc_hex(cfg.chain);res.tip=xgtx_rpc_hex(snapshot.tip);res.treasury=xgtx_rpc_hex(cfg.treasury);
    res.height=snapshot.height;res.activation_height=cfg.activation_height;res.native_backing=cfg.native_backing;res.native_locked=std::to_string(snapshot.native_locked);res.reserve_outputs=std::to_string(cfg.native_backing?db.get_num_outputs(xgtx_exchange::native_reserve_bucket):0);
    if(owner){auto it=snapshot.nonces.find(*owner);auto nonce=it==snapshot.nonces.end()?0:it->second;if(nonce==std::numeric_limits<xgtx_exchange::amount>::max())throw std::invalid_argument("Owner nonce exhausted");res.next_nonce=std::to_string(nonce+1);}
    auto asset=asset_cursor?snapshot.assets.upper_bound(*asset_cursor):snapshot.assets.begin();
    for(std::uint32_t n=0;asset!=snapshot.assets.end() && n<req.limit;++asset,++n){
      xgtx_rpc_asset row;row.id=xgtx_rpc_hex(asset->first);row.issuer=xgtx_rpc_hex(asset->second.issuer);row.supply=std::to_string(asset->second.supply);row.ticker=asset->second.ticker;row.decimals=asset->second.decimals;
      if(owner){auto it=snapshot.balances.find({*owner,asset->first});row.balance=std::to_string(it==snapshot.balances.end()?0:it->second);}res.assets.push_back(std::move(row));
    }
    if(asset!=snapshot.assets.end() && !res.assets.empty())res.next_asset=res.assets.back().id;
    auto pool=pool_cursor?snapshot.pools.upper_bound(*pool_cursor):snapshot.pools.begin();
    for(std::uint32_t n=0;pool!=snapshot.pools.end() && n<req.limit;++pool,++n){
      xgtx_rpc_pool row;row.id=xgtx_rpc_hex(pool->first);row.asset_a=xgtx_rpc_hex(pool->second.a);row.asset_b=xgtx_rpc_hex(pool->second.b);row.reserve_a=std::to_string(pool->second.reserve_a);row.reserve_b=std::to_string(pool->second.reserve_b);row.shares=std::to_string(pool->second.shares);
      if(owner){auto it=snapshot.lp.find({*owner,pool->first});row.owner_shares=std::to_string(it==snapshot.lp.end()?0:it->second);}res.pools.push_back(std::move(row));
    }
    if(pool!=snapshot.pools.end() && !res.pools.empty())res.next_pool=res.pools.back().id;
    res.status=CORE_RPC_STATUS_OK;return true;
  }catch(const std::invalid_argument& failure){error.code=CORE_RPC_ERROR_CODE_WRONG_PARAM;error.message=failure.what();return false;}
  catch(const std::exception& failure){error.code=CORE_RPC_ERROR_CODE_INTERNAL_ERROR;error.message=failure.what();return false;}
}
bool core_rpc_server::on_xgtx_quote_swap(const COMMAND_RPC_XGTX_QUOTE_SWAP::request& req,COMMAND_RPC_XGTX_QUOTE_SWAP::response& res,epee::json_rpc::error& error,const connection_context*){
  RPC_TRACKER(xgtx_quote_swap);
  try{
    auto in=xgtx_rpc_id(req.asset_in),out=xgtx_rpc_id(req.asset_out);auto quantity=xgtx_rpc_amount(req.amount_in);
    auto& blockchain=m_core.get_blockchain_storage();CRITICAL_REGION_LOCAL(blockchain);
    const auto& db=blockchain.get_db();const auto* exchange=db.experimental_exchange_engine();
    if(!exchange){error.code=-32601;error.message="Exchange is not enabled on this node";return false;}
    const auto snapshot=db.experimental_exchange_state();xgtx_rpc_check_snapshot(snapshot,db);
    auto quote=exchange->quote_swap(snapshot,in,out,quantity);
    res.chain=xgtx_rpc_hex(exchange->settings().chain);res.tip=xgtx_rpc_hex(snapshot.tip);res.height=snapshot.height;res.pool=xgtx_rpc_hex(quote.pool_id);
    res.amount_in=std::to_string(quantity);res.amount_out=std::to_string(quote.output);res.lp_fee=std::to_string(quote.lp_fee);res.protocol_fee=std::to_string(quote.protocol_fee);res.status=CORE_RPC_STATUS_OK;return true;
  }catch(const std::invalid_argument& failure){error.code=CORE_RPC_ERROR_CODE_WRONG_PARAM;error.message=failure.what();return false;}
  catch(const std::exception& failure){error.code=CORE_RPC_ERROR_CODE_WRONG_PARAM;error.message=failure.what();return false;}
}
}
