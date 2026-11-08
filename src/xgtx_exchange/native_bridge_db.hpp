#pragma once
#include "native_withdraw.hpp"
#include "blockchain_db/blockchain_db.h"
#include "cryptonote_config.h"
namespace xgtx_exchange {
static_assert(native_reserve_bucket>MONEY_SUPPLY,"reserve bucket must exceed every legitimate native amount");
static_assert(wide(native_reserve_bucket)*16<=std::numeric_limits<amount>::max(),"reserve input metadata must preserve native overflow checks");
inline native_effect withdrawal_effect_from_database(const cryptonote::transaction& tx,const engine& e,const cryptonote::BlockchainDB& db){
  return withdrawal_native_effect(tx,e,[&](std::uint64_t index){
    auto origin=db.get_output_tx_and_index(reserve_policy::bucket,index);
    auto record=db.get_output_key(reserve_policy::bucket,index,true);
    native_reserve_source source;source.origin=db.get_pruned_tx(origin.first);std::memcpy(source.transaction.data(),&origin.first,32);
    crypto::hash prunable{};carrier_require(db.get_prunable_tx_hash(origin.first,prunable),"missing reserve origin prunable hash");std::memcpy(source.prunable_hash.data(),&prunable,32);
    carrier_require(origin.second<=std::numeric_limits<std::uint32_t>::max(),"reserve origin index overflow");source.index=origin.second;
    source.stored={rct::pk2rct(record.pubkey),record.commitment};return source;
  },[&](const crypto::key_image& image){return db.has_key_image(image);});
}
// Derive effects exclusively from the authenticated native transaction and DB.
// This is additional validation; callers must still run normal native checks.
inline std::optional<native_effect> transaction_effect_from_database(
    const cryptonote::transaction& tx,const engine& e,const cryptonote::BlockchainDB& db){
  auto intent=deposit_carrier(tx,e.settings().chain);
  if(!intent){reserve_policy::reject_unreleased_inputs(tx);return std::nullopt;}
  carrier_require(e.settings().native_backing,"native backing disabled");
  if(intent->op.type==kind::native_deposit)
    return deposit_native_effect(tx,e,db.get_num_outputs(reserve_policy::bucket));
  return withdrawal_effect_from_database(tx,e,db);
}
}
