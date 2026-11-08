#pragma once
#include <string>
#include <cstdint>
#include "serialization/keyvalue_serialization.h"
namespace tools { namespace wallet_rpc {
struct COMMAND_RPC_XGTX_OPERATION {
  struct request {
    std::uint32_t kind=0,priority=0,decimals=0,deadline_window=20;
    std::string asset,other,recipient,quantity,limit="0",minimum="0",ticker,max_fee;
    bool do_not_relay=true,bootstrap=false;
    BEGIN_KV_SERIALIZE_MAP()
      KV_SERIALIZE(kind) KV_SERIALIZE(quantity) KV_SERIALIZE(max_fee)
      KV_SERIALIZE_OPT(priority,(std::uint32_t)0) KV_SERIALIZE_OPT(decimals,(std::uint32_t)0)
      KV_SERIALIZE_OPT(deadline_window,(std::uint32_t)20) KV_SERIALIZE_OPT(asset,std::string())
      KV_SERIALIZE_OPT(other,std::string()) KV_SERIALIZE_OPT(recipient,std::string())
      KV_SERIALIZE_OPT(limit,std::string("0")) KV_SERIALIZE_OPT(minimum,std::string("0"))
      KV_SERIALIZE_OPT(ticker,std::string()) KV_SERIALIZE_OPT(do_not_relay,true) KV_SERIALIZE_OPT(bootstrap,false)
    END_KV_SERIALIZE_MAP()
  };
  struct response {
    std::string tx_hash,tx_blob,fee,weight,chain,owner,nonce,deadline,asset_id,signed_intent,payout,draft_id,deposit_quantity;
    bool relayed=false;
    BEGIN_KV_SERIALIZE_MAP()
      KV_SERIALIZE(tx_hash) KV_SERIALIZE(tx_blob) KV_SERIALIZE(fee) KV_SERIALIZE(weight)
      KV_SERIALIZE(chain) KV_SERIALIZE(owner) KV_SERIALIZE(nonce) KV_SERIALIZE(deadline)
      KV_SERIALIZE(deposit_quantity) KV_SERIALIZE(draft_id) KV_SERIALIZE(asset_id) KV_SERIALIZE(payout) KV_SERIALIZE(signed_intent) KV_SERIALIZE(relayed)
    END_KV_SERIALIZE_MAP()
  };
};
struct COMMAND_RPC_XGTX_IDENTITY {
  struct request { BEGIN_KV_SERIALIZE_MAP() END_KV_SERIALIZE_MAP() };
  struct response {
    std::string address,owner,chain;
    BEGIN_KV_SERIALIZE_MAP() KV_SERIALIZE(address) KV_SERIALIZE(owner) KV_SERIALIZE(chain) END_KV_SERIALIZE_MAP()
  };
};
struct COMMAND_RPC_XGTX_RELAY_OPERATION {
  struct request {
    std::string draft_id;
    BEGIN_KV_SERIALIZE_MAP() KV_SERIALIZE(draft_id) END_KV_SERIALIZE_MAP()
  };
  using response=COMMAND_RPC_XGTX_OPERATION::response;
};

}}
