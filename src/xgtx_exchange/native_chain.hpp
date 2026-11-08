#pragma once
#include "native_carrier.hpp"
#include "activation_policy.hpp"
#include "reserve_guard.hpp"
#include "native_deposit.hpp"
#include "native_bridge_db.hpp"
#include "lmdb_store.hpp"

namespace xgtx_exchange {
inline id native_id(const crypto::hash& hash){id out{};std::memcpy(out.data(),&hash,32);return out;}
// Experimental chain projection. Engine heights/deadlines count blocks:
// genesis is ordinal 1; native database height is already a block count.
// Native proof validation remains the caller's responsibility.
inline std::vector<envelope> block_intents(const cryptonote::block& block,
    const std::vector<std::pair<cryptonote::transaction,cryptonote::blobdata>>& txs,
    const engine& e){
  carrier_require(block.tx_hashes.size()==txs.size(),"native transaction count mismatch");
  auto count=[](const cryptonote::transaction& tx){
    extra_fields fields;carrier_require(cryptonote::parse_tx_extra(tx.extra,fields),"malformed transaction extra");
    std::size_t n=0;for(const auto& f:fields)if(f.type()==typeid(cryptonote::tx_extra_xgtx_exchange))++n;return n;
  };
  carrier_require(count(block.miner_tx)==0,"exchange operation in coinbase");
  std::vector<envelope> out;
  for(const auto& tx:txs){
    // A reserve input requires a withdrawal carrier and enabled backing. All
    // actual reserve amounts/authorization are validated before ledger append.
    if(count(tx.first)){
      carrier_require(out.size()<e.settings().max_operations,"native exchange operation cap");
      auto intent=verify_native_carrier(tx.first,e.settings().chain);
      if(reserve_policy::has_reserved_input(tx.first))carrier_require(e.settings().native_backing && intent.op.type==kind::native_withdraw,"reserve spend without withdrawal authorization");
      out.push_back(intent);
    }else reserve_policy::reject_unreleased_inputs(tx.first);
  }
  return out;
}
inline void append_native_block(MDB_txn* txn,const engine& e,const cryptonote::block& block,
    const std::vector<std::pair<cryptonote::transaction,cryptonote::blobdata>>& txs,
    std::uint64_t previous_block_count,std::uint64_t existing_reserves=0,const cryptonote::BlockchainDB* native_db=nullptr){
  carrier_require(txn!=nullptr,"missing native write transaction");
  const auto hash=native_id(cryptonote::get_block_hash(block));
  const auto parent=native_id(block.prev_id);
  if(previous_block_count==0)carrier_require(hash==e.settings().chain && parent==id{},"wrong experimental genesis");
  // Before the declared fork, opaque legacy extra has no exchange meaning.
  // Do not retroactively impose carrier/extra validation on native history.
  if(!active_for_next_block(e.settings(),previous_block_count)){
    lmdb_store store(txn,e);auto before=store.load(txn);
    carrier_require(before.height==previous_block_count,"pre-activation projection height mismatch");
    store.append(txn,previous_block_count+1,parent,hash,{});return;
  }
  auto ops=block_intents(block,txs,e);
  lmdb_store store(txn,e);auto before=store.load(txn);
  carrier_require(before.height==previous_block_count,"native/exchange height mismatch; resync isolated development chain");
  if(!e.settings().native_backing)store.append(txn,previous_block_count+1,parent,hash,ops);
  else {
    std::vector<native_block_operation> verified;verified.reserve(ops.size());
    for(std::size_t i=0;i<txs.size();++i){
      const auto& tx=txs[i].first;extra_fields fields;
      carrier_require(cryptonote::parse_tx_extra(tx.extra,fields),"malformed native extra");
      bool carrier=false;for(const auto& field:fields)if(field.type()==typeid(cryptonote::tx_extra_xgtx_exchange))carrier=true;
      if(!carrier)continue;
      carrier_require(cryptonote::get_transaction_hash(tx)==block.tx_hashes[i],"native carrier transaction hash mismatch");
      carrier_require(cryptonote::tx_to_blob(tx)==txs[i].second,"native carrier blob mismatch");
      auto intent=verify_native_carrier(tx,e.settings().chain);
      if(intent.op.type==kind::native_deposit)verified.push_back({intent,deposit_native_effect(tx,e,existing_reserves)});
      else if(intent.op.type==kind::native_withdraw){carrier_require(native_db!=nullptr,"missing native reserve database");verified.push_back({intent,withdrawal_effect_from_database(tx,e,*native_db)});}
      else verified.push_back({intent,std::nullopt});
    }
    store.append_verified(txn,previous_block_count+1,parent,hash,verified);
  }
}
inline void pop_native_block(MDB_txn* txn,const engine& e,const cryptonote::block& block,std::uint64_t block_count){
  carrier_require(txn!=nullptr,"missing native write transaction");
  lmdb_store store(txn,e);auto before=store.load(txn);
  carrier_require(before.height==block_count,"native/exchange rollback height mismatch");
  store.pop(txn,native_id(cryptonote::get_block_hash(block)));
}
inline bool has_exchange_projection(MDB_txn* txn){
  MDB_dbi db{};int rc=mdb_dbi_open(txn,"xgtx_exchange_state_v1",0,&db);
  if(rc==MDB_NOTFOUND)return false;
  if(rc)throw std::runtime_error(mdb_strerror(rc));
  return true;
}
inline state read_native_projection(MDB_txn* txn,const engine& e){
  MDB_dbi db{};int rc=mdb_dbi_open(txn,"xgtx_exchange_state_v1",0,&db);
  if(rc)throw std::runtime_error(mdb_strerror(rc));
  std::string key="current";MDB_val k{key.size(),const_cast<char*>(key.data())},v{};
  rc=mdb_get(txn,db,&k,&v);if(rc)throw std::runtime_error(mdb_strerror(rc));
  auto p=static_cast<const std::uint8_t*>(v.mv_data);auto s=deserialize(bytes(p,p+v.mv_size));e.invariants(s);return s;
}
}
