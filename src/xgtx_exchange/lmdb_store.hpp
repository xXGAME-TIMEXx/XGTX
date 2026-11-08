#pragma once
#include "engine.hpp"
#include "state_undo.hpp"
#include <algorithm>
#include <lmdb.h>

namespace xgtx_exchange {
// Operates inside the caller's existing LMDB transaction. No second database,
// transaction commit, or independently committed sidecar is used.
class lmdb_store {
 public:
  explicit lmdb_store(MDB_txn* txn,const engine& e):engine_(e){
    fail(mdb_dbi_open(txn,"xgtx_exchange_state_v1",MDB_CREATE,&db_));
    bytes policy={'X','G','T','X','P','O','L',0,1};auto& c=e.settings();if(c.native_backing){policy.back()=3;policy.push_back(1);for(auto n:{native_reserve_bucket,amount(16)})for(unsigned i=0;i<8;++i)policy.push_back(static_cast<std::uint8_t>(n>>(8*i)));}
    policy.insert(policy.end(),c.chain.begin(),c.chain.end());policy.insert(policy.end(),c.treasury.begin(),c.treasury.end());
    for(auto n:{c.activation_height,static_cast<amount>(c.max_operations)})for(unsigned i=0;i<8;++i)policy.push_back(static_cast<std::uint8_t>(n>>(8*i)));
    auto legacy=policy;policy[8]=4;policy.insert(policy.end(),{'U','D',1,legacy[8]});policy_=policy;
    auto found=read(txn,"policy");if(found.empty()){write(txn,"policy",policy);write(txn,"current",serialize(state{}));}
    else if(found!=policy && found!=legacy)throw std::invalid_argument("exchange configuration mismatch");
  }
  state load(MDB_txn* txn)const {auto b=read(txn,"current");if(b.empty())throw std::runtime_error("missing exchange state");auto s=deserialize(b);engine_.invariants(s);return s;}
  block_result append(MDB_txn* txn,amount height,const id& parent,const id& block,const std::vector<envelope>& ops)const {
    if(ops.size()>engine_.settings().max_operations)throw std::invalid_argument("block operation cap");
    std::vector<native_block_operation> verified;verified.reserve(ops.size());
    for(const auto& op:ops)verified.push_back({op,std::nullopt});
    return append_verified(txn,height,parent,block,verified);
  }
  block_result append_verified(MDB_txn* txn,amount height,const id& parent,const id& block,const std::vector<native_block_operation>& ops)const {
    auto before=load(txn);auto r=engine_.apply_verified_block(before,height,parent,block,ops);
    auto journal=encode_state_undo(before,r.next,engine_);if(journal.size()>64*1024*1024)journal=serialize(before);bytes undo(block.begin(),block.end());undo.insert(undo.end(),journal.begin(),journal.end());
    auto key=undo_key(height);if(!read(txn,key).empty())throw std::invalid_argument("existing undo at height");
    write(txn,"policy",policy_);write(txn,key,undo);write(txn,"current",serialize(r.next));return r;
  }
  state pop(MDB_txn* txn,const id& expected_tip)const {
    auto current=load(txn);if(!current.height || current.tip!=expected_tip)throw std::invalid_argument("rollback tip mismatch");
    auto key=undo_key(current.height);auto undo=read(txn,key);if(undo.size()<32 || !std::equal(current.tip.begin(),current.tip.end(),undo.begin()))throw std::runtime_error("invalid block undo");
    bytes payload(undo.begin()+32,undo.end());
    auto prior=is_state_undo(payload)?restore_state_undo(current,payload,engine_):deserialize(payload);engine_.invariants(prior);
    if(prior.height+1!=current.height)throw std::runtime_error("invalid undo height");
    write(txn,"policy",policy_);write(txn,"current",serialize(prior));MDB_val k{key.size(),const_cast<char*>(key.data())};fail(mdb_del(txn,db_,&k,nullptr));return prior;
  }
 private:
  const engine& engine_;MDB_dbi db_{};bytes policy_;
  static void fail(int rc){if(rc)throw std::runtime_error(mdb_strerror(rc));}
  static std::string undo_key(amount h){std::string k="undo/";for(unsigned i=0;i<8;++i)k.push_back(static_cast<char>(h>>(8*i)));return k;}
  bytes read(MDB_txn* txn,const std::string& key)const {MDB_val k{key.size(),const_cast<char*>(key.data())},v{};int rc=mdb_get(txn,db_,&k,&v);if(rc==MDB_NOTFOUND)return {};fail(rc);auto p=static_cast<const std::uint8_t*>(v.mv_data);return bytes(p,p+v.mv_size);}
  void write(MDB_txn* txn,const std::string& key,const bytes& data)const {MDB_val k{key.size(),const_cast<char*>(key.data())},v{data.size(),const_cast<std::uint8_t*>(data.data())};fail(mdb_put(txn,db_,&k,&v,0));}
};
}
