#pragma once
#include "reserve_proofs.hpp"
#include <optional>
namespace xgtx_exchange {
inline std::optional<envelope> deposit_carrier(const cryptonote::transaction& tx,const id& chain){
  extra_fields fields;carrier_require(cryptonote::parse_tx_extra(tx.extra,fields),"malformed native extra");
  bool found=false;for(const auto& field:fields)if(field.type()==typeid(cryptonote::tx_extra_xgtx_exchange))found=true;
  if(!found)return std::nullopt;
  auto intent=verify_native_carrier(tx,chain);
  if(intent.op.type!=kind::native_deposit && intent.op.type!=kind::native_withdraw)return std::nullopt;
  return intent;
}
inline amount deposit_part(amount total,std::size_t count,std::size_t index){
  carrier_require((count==1 || count==16) && index<count && total>=count,"invalid native deposit split");
  return total/count+(index<total%count?1:0);
}
// Deterministic classification is used on both insertion and pop. Public keys
// derived by reserve_proofs MUST be segregated before any ledger credit commits.
inline std::vector<std::uint8_t> deposit_output_mask(const cryptonote::transaction& tx,const id& chain){
  std::vector<std::uint8_t> mask(tx.vout.size(),0);auto intent=deposit_carrier(tx,chain);if(!intent)return mask;
  if(intent->op.type==kind::native_withdraw){
    carrier_require(tx.vout.size()==2,"native withdrawal output count");
    if(intent->op.limit){reserve_proofs::domain domain{chain,intent->op.sender,intent->op.nonce};reserve_proofs::inspect_output(tx,domain,0,intent->op.limit);mask[0]=1;}
    return mask;
  }
  const auto count=intent->op.limit;
  carrier_require((count==1 || count==16) && tx.vout.size()>=count && tx.vout.size()<=16,"native reserve output count");
  if(count==16)carrier_require(tx.vout.size()==16,"bootstrap deposit requires sixteen reserve outputs");
  reserve_proofs::domain domain{chain,intent->op.sender,intent->op.nonce};
  for(std::size_t i=0;i<tx.vout.size();++i){
    if(i<count){reserve_proofs::inspect_output(tx,domain,i,deposit_part(intent->op.quantity,count,i));mask[i]=1;}
    else {crypto::public_key key{};carrier_require(cryptonote::get_output_public_key(tx.vout[i],key) && key!=reserve_proofs::derive(domain,i).public_key,"undeclared public reserve key");}
  }
  return mask;
}
// Native DB admission already assumes the caller performed the chain's normal
// input/range/unlock/key-image consensus validation. This function authenticates
// the additional reserve effect; it does NOT replace those native checks.
inline native_effect deposit_native_effect(const cryptonote::transaction& tx,const engine& e,std::uint64_t existing_reserves){
  carrier_require(e.settings().native_backing,"native backing disabled");
  reserve_policy::reject_unreleased_inputs(tx);
  const auto intent=deposit_carrier(tx,e.settings().chain);carrier_require(bool(intent) && intent->op.type==kind::native_deposit,"missing native deposit");
  carrier_require(intent->op.limit==16 || existing_reserves>=16,"first deposit must bootstrap sixteen reserve outputs");
  deposit_output_mask(tx,e.settings().chain);
  carrier_require(rct::verRctSemanticsSimple(tx.rct_signatures),"native deposit range/conservation proofs invalid");
  auto hash=cryptonote::get_transaction_hash(tx);id transaction{};std::memcpy(transaction.data(),&hash,32);
  return {transaction,cryptonote_hash(encode_envelope(*intent)),{},intent->op.quantity,0};
}
}
