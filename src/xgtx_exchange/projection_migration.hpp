#pragma once
#include "lmdb_store.hpp"
#include <memory>

namespace xgtx_exchange {
// Historical native validation is not repeated here. The adapter must supply
// authenticated native block hashes and refuse exchange/reserve-bearing history.
// Caller owns one write transaction and must abort it on any failure.
struct migration_block { id hash{},parent{}; bool ordinary_history=false; };
inline bool projection_table_exists(MDB_txn* txn){
  MDB_dbi db{};const int rc=mdb_dbi_open(txn,"xgtx_exchange_state_v1",0,&db);
  if(rc==MDB_NOTFOUND)return false;
  if(rc)throw std::runtime_error(mdb_strerror(rc));
  return true;
}
template<class Fetch> state stage_pre_activation_projection(MDB_txn* txn,const engine& e,
    amount count,const id& expected_tip,Fetch fetch){
  if(!txn)throw std::invalid_argument("missing migration write transaction");
  if(!count || count>100000)throw std::invalid_argument("migration history count outside rehearsal limit");
  if(e.settings().chain==id{} || expected_tip==id{})throw std::invalid_argument("missing migration identity");
  if(e.settings().activation_height<=count)throw std::invalid_argument("migration must finish strictly before activation");
  if(projection_table_exists(txn))throw std::invalid_argument("existing exchange table: migration refuses overwrite or resume");
  lmdb_store store(txn,e);
  for(amount index=0;index<count;++index){
    const auto block=fetch(index);
    if(!block.ordinary_history)throw std::invalid_argument("historical exchange, reserve or uninterpretable transaction requires reviewed migration");
    if(!index && (block.hash!=e.settings().chain || block.parent!=id{}))throw std::invalid_argument("migration genesis mismatch");
    store.append(txn,index+1,block.parent,block.hash,{});
  }
  auto result=store.load(txn);
  if(result.height!=count || result.tip!=expected_tip)throw std::invalid_argument("migration final tip mismatch");
  auto empty=result;empty.height=0;empty.tip={};
  if(!(empty==state{}))throw std::runtime_error("migration fabricated economic state");
  return result;
}
// Verify every persisted undo record in a caller-aborted transaction. Native
// blocks are never popped. The caller must abort even after successful audit.
template<class Fetch> void audit_pre_activation_projection_undo(MDB_txn* txn,
    const engine& e,amount count,const id& tip,Fetch hash_at){
  if(!count || count>100000 || e.settings().activation_height<=count || !projection_table_exists(txn))
    throw std::invalid_argument("invalid migration undo audit bounds");
  lmdb_store store(txn,e);auto current=store.load(txn);
  if(current.height!=count || current.tip!=tip)throw std::invalid_argument("migration undo audit tip mismatch");
  for(amount n=count;n>0;--n)store.pop(txn,hash_at(n-1));
  if(!(store.load(txn)==state{}))throw std::runtime_error("migration undo did not restore empty origin");
}
// Stable commitment to every named native table and every duplicate value.
// Excludes only the projection table; page numbers and root metadata are not
// economic data and necessarily change when adding a named table.
inline id native_tables_migration_commitment(MDB_txn* txn,const hasher& hash){
  if(!txn || !hash)throw std::invalid_argument("missing commitment transaction/hash");
  id digest{};
  auto absorb=[&](unsigned tag,const MDB_val& key,const MDB_val& value){
    bytes frame(digest.begin(),digest.end());frame.push_back(static_cast<uint8_t>(tag));
    for(const auto* item:{&key,&value}){
      const uint64_t size=item->mv_size;
      for(unsigned i=0;i<8;++i)frame.push_back(static_cast<uint8_t>(size>>(8*i)));
      if(size){const auto* p=static_cast<const uint8_t*>(item->mv_data);frame.insert(frame.end(),p,p+size);}
    }
    digest=hash(frame);
  };
  auto fail=[](int rc){if(rc)throw std::runtime_error(mdb_strerror(rc));};
  MDB_dbi main{};fail(mdb_dbi_open(txn,nullptr,0,&main));
  MDB_cursor* raw=nullptr;fail(mdb_cursor_open(txn,main,&raw));
  std::unique_ptr<MDB_cursor,decltype(&mdb_cursor_close)> names(raw,mdb_cursor_close);
  MDB_val name{},descriptor{};int rc=mdb_cursor_get(names.get(),&name,&descriptor,MDB_FIRST);
  while(rc==0){
    const std::string table(static_cast<const char*>(name.mv_data),name.mv_size);
    if(table!="xgtx_exchange_state_v1"){
      MDB_dbi db{};fail(mdb_dbi_open(txn,table.c_str(),0,&db));unsigned flags=0;fail(mdb_dbi_flags(txn,db,&flags));
      std::array<uint8_t,4> encoded_flags{};for(unsigned i=0;i<4;++i)encoded_flags[i]=flags>>(8*i);
      MDB_val flag_value{encoded_flags.size(),encoded_flags.data()};absorb(1,name,flag_value);
      raw=nullptr;fail(mdb_cursor_open(txn,db,&raw));
      std::unique_ptr<MDB_cursor,decltype(&mdb_cursor_close)> records(raw,mdb_cursor_close);
      MDB_val key{},value{};int entry=mdb_cursor_get(records.get(),&key,&value,MDB_FIRST);
      while(entry==0){absorb(2,key,value);entry=mdb_cursor_get(records.get(),&key,&value,MDB_NEXT);}
      if(entry!=MDB_NOTFOUND)fail(entry);
      MDB_val empty{};absorb(3,empty,empty);
    }
    rc=mdb_cursor_get(names.get(),&name,&descriptor,MDB_NEXT);
  }
  if(rc!=MDB_NOTFOUND)fail(rc);
  return digest;
}
}
