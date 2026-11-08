#pragma once
#include "native_carrier.hpp"
#include "native_bridge_db.hpp"
#include <limits>
#include <optional>

namespace xgtx_exchange {
inline std::optional<operation> pending_operation(const cryptonote::transaction_prefix& tx){
  extra_fields fields;
  carrier_require(cryptonote::parse_tx_extra(tx.extra,fields),"malformed pending extra");
  std::size_t count=0,index=0;
  for(std::size_t i=0;i<fields.size();++i)if(fields[i].type()==typeid(cryptonote::tx_extra_xgtx_exchange)){++count;index=i;}
  if(!count)return std::nullopt;
  carrier_require(count==1 && tx.extra.size()<=16384,"invalid pending carrier count/size");
  return decode_carrier(boost::get<cryptonote::tx_extra_xgtx_exchange>(fields[index]).data).intent.op;
}
inline std::optional<envelope> transaction_intent(const cryptonote::transaction& tx,const id& chain){
  if(!pending_operation(tx))return std::nullopt;
  return verify_native_carrier(tx,chain);
}
struct pending_operation_record { id transaction{};operation op; };
inline amount next_ordinal(const state& confirmed){
  carrier_require(confirmed.height<std::numeric_limits<amount>::max(),"exchange height overflow");
  return confirmed.height+1;
}
// Conservative relay policy: validate against confirmed state, without spending
// pending credits, and reserve an owner's next nonce for one native transaction.
// Block consensus is more permissive: ordered dependent operations are allowed.
inline void validate_admission(const engine& e,const state& confirmed,const envelope& candidate,
    const id& transaction,const std::vector<pending_operation_record>& pending,
    const std::optional<native_effect>& effect=std::nullopt){
  auto preview=confirmed;
  if(effect)e.apply_verified_native(preview,candidate,next_ordinal(confirmed),*effect);
  else e.apply(preview,candidate,next_ordinal(confirmed));
  for(const auto& record:pending){
    if(record.transaction==transaction)continue; // relay-method update of same tx
    carrier_require(!(record.op.chain==e.settings().chain && record.op.sender==candidate.op.sender &&
      record.op.nonce==candidate.op.nonce),"pending exchange nonce conflict");
  }
}
class template_view {
 public:
  template_view(const engine& e,const state& confirmed):engine_(e),candidate_(confirmed),height_(next_ordinal(confirmed)){}
  bool include(const cryptonote::transaction& tx,const std::optional<native_effect>& effect=std::nullopt){
    auto intent=transaction_intent(tx,engine_.settings().chain);
    if(!intent){carrier_require(!effect,"native effect without carrier");reserve_policy::reject_unreleased_inputs(tx);return false;}
    carrier_require(operations_<engine_.settings().max_operations,"template exchange operation cap");
    if(effect)engine_.apply_verified_native(candidate_,*intent,height_,*effect);
    else engine_.apply(candidate_,*intent,height_);
    ++operations_;return true;
  }
  const state& candidate()const{return candidate_;}
  std::size_t operations()const{return operations_;}
 private:
  const engine& engine_;state candidate_;amount height_;std::size_t operations_=0;
};
}
