#include "../../src/xgtx_exchange/engine.hpp"
#include "../../src/xgtx_exchange/lmdb_store.hpp"
#ifdef XGTX_TEST_CRYPTONOTE
#include "../xgtx_exchange/test_native_crypto.hpp"
#else
#include "../xgtx_exchange/test_crypto.hpp"
#endif
#include <filesystem>
#include <iostream>
#include <cstdlib>
using namespace xgtx_exchange;
namespace {
unsigned checks=0;
void check(bool good,const char* reason){++checks;if(!good)throw std::runtime_error(reason);}
template<class F>void rejects(F fn,const char* reason){bool failed=false;try{fn();}catch(const std::exception&){failed=true;}check(failed,reason);}
id key(unsigned n){id k{};for(unsigned i=0;i<4;++i)k[i]=n>>(8*i);return k;}
amount balance(const state& s,const id& who){auto it=s.balances.find({who,native_asset});return it==s.balances.end()?0:it->second;}
native_effect effect(const envelope& op,amount locked,amount released,id payout={}){
  return {key(800+unsigned(op.op.nonce)),xgtx_test::hash(encode_envelope(op)),payout,locked,released};
}
void rejected(engine& engine,state& s,const envelope& op,const native_effect& proof,const char* label){
  auto before=serialize(s);rejects([&]{engine.apply_verified_native(s,op,1,proof);},label);check(serialize(s)==before,"failed operation changed ledger");
}
struct directory {
  std::filesystem::path path;
  directory(){std::string name=(std::filesystem::temp_directory_path()/"xgtx-backed-ledger-XXXXXX").string();auto p=mkdtemp(name.data());if(!p)throw std::runtime_error("temporary directory");path=p;}
  ~directory(){std::error_code error;std::filesystem::remove_all(path,error);}
};
struct database {
  MDB_env* env=nullptr;directory dir;
  database(){if(mdb_env_create(&env) || mdb_env_set_maxdbs(env,4) || mdb_env_set_mapsize(env,16*1024*1024) || mdb_env_open(env,dir.path.c_str(),0,0600))throw std::runtime_error("LMDB open");}
  ~database(){if(env)mdb_env_close(env);}
  void reopen(){mdb_env_close(env);env=nullptr;if(mdb_env_create(&env) || mdb_env_set_maxdbs(env,4) || mdb_env_set_mapsize(env,16*1024*1024) || mdb_env_open(env,dir.path.c_str(),0,0600))throw std::runtime_error("LMDB reopen");}
  template<class F>void write(F fn,bool commit=true){MDB_txn* txn=nullptr;if(mdb_txn_begin(env,nullptr,0,&txn))throw std::runtime_error("LMDB begin");try{fn(txn);}catch(...){mdb_txn_abort(txn);throw;}if(commit){if(mdb_txn_commit(txn))throw std::runtime_error("LMDB commit");}else mdb_txn_abort(txn);}
};
}
int main(){try{
  xgtx_test::wallet alice(1),bob(2),treasury(3);const id chain=key(77);
  config cfg{chain,treasury.pub,1,1024,true};engine e(cfg,xgtx_test::hash,xgtx_test::verify);
  auto disabled_cfg=cfg;disabled_cfg.native_backing=false;engine disabled(disabled_cfg,xgtx_test::hash,xgtx_test::verify);
  operation dep;dep.type=kind::native_deposit;dep.chain=chain;dep.deadline=100;dep.quantity=1000000;dep.limit=16;
  const auto deposit=alice.sign(dep);const auto proof=effect(deposit,1000000,0);
  check(encode_envelope(decode_envelope(encode_envelope(deposit)))==encode_envelope(deposit),"deposit signed codec");
  state s;auto old=serialize(s);rejects([&]{e.apply(s,deposit,1);},"ordinary entry point cannot mint native credit");check(serialize(s)==old,"unverified deposit rejection atomic");
  rejected(disabled,s,deposit,proof,"native backing remains disabled by default");
  auto bad=proof;bad.locked++;rejected(e,s,deposit,bad,"incorrect locked value");
  bad=proof;bad.released=1;rejected(e,s,deposit,bad,"deposit cannot release reserve");
  bad=proof;bad.intent[0]^=1;rejected(e,s,deposit,bad,"native evidence bound to exact signed intent");
  bad=proof;bad.transaction={};rejected(e,s,deposit,bad,"missing native transaction");
  bad=proof;bad.payout=bob.pub;rejected(e,s,deposit,bad,"deposit has no payout");
  auto forged=deposit;forged.sig[0]^=1;rejected(e,s,forged,effect(forged,1000000,0),"forged owner signature");
  e.apply_verified_native(s,deposit,1,proof);check(s.native_locked==1000000 && balance(s,alice.pub)==1000000,"deposit creates exactly backed native balance");
  check(s.assets.at(native_asset).supply==s.native_locked,"native supply equals locked reserve");
  check(serialize(s)[7]==2 && deserialize(serialize(s))==s,"backed state version 2 roundtrip");
  rejected(e,s,deposit,proof,"deposit replay");rejects([&]{disabled.invariants(s);},"disabled engine rejects backed state");
  auto corrupt=s;corrupt.native_locked++;rejects([&]{e.invariants(corrupt);},"native backing mismatch rejected");
  operation create;create.chain=chain;create.deadline=100;create.nonce=2;create.quantity=1000000;create.ticker="TOKEN";
  auto issue=alice.sign(create);auto token=e.apply(s,issue,1).asset;
  operation pool;pool.type=kind::pool_create;pool.chain=chain;pool.deadline=100;pool.nonce=3;pool.asset=native_asset;pool.other=token;pool.quantity=500000;pool.limit=500000;
  auto pool_receipt=e.apply(s,alice.sign(pool),1);check(s.pools.size()==1 && pool_receipt.shares==500000,"native XGTX/token pool");
  operation swap;swap.type=kind::swap;swap.chain=chain;swap.deadline=100;swap.nonce=4;swap.asset=native_asset;swap.other=token;swap.quantity=100000;swap.minimum=1;
  auto before_quote=serialize(s);auto native_quote=e.quote_swap(s,native_asset,token,100000);
  check(serialize(s)==before_quote,"native quote never changes backing or balances");
  rejects([&]{disabled.quote_swap(s,native_asset,token,100000);},"disabled backing rejects native quote");
  rejects([&]{e.quote_swap(s,native_asset,token,0);},"zero input quote rejects");
  rejects([&]{e.quote_swap(s,token,token,1);},"same asset quote rejects");
  auto trade=e.apply(s,alice.sign(swap),1);check(native_quote.output==trade.output && native_quote.lp_fee==trade.lp_fee && native_quote.protocol_fee==trade.protocol_fee,"native quote matches confirmed execution");check(trade.lp_fee==250 && trade.protocol_fee==50,"native input swap fee split");
  check(balance(s,treasury.pub)==50 && s.native_locked==1000000,"treasury credit stays fully backed");
  operation withdraw;withdraw.type=kind::native_withdraw;withdraw.chain=chain;withdraw.deadline=100;withdraw.nonce=5;withdraw.quantity=200000;withdraw.limit=100000;withdraw.recipient=bob.pub;
  auto redeem=alice.sign(withdraw);auto release=effect(redeem,100000,300000,bob.pub);
  check(encode_envelope(decode_envelope(encode_envelope(redeem)))==encode_envelope(redeem),"withdrawal signed codec");
  bad=release;bad.payout=alice.pub;rejected(e,s,redeem,bad,"unauthorized payout");
  bad=release;bad.released++;rejected(e,s,redeem,bad,"reserve decrease must match ledger debit");
  bad=release;bad.locked++;rejected(e,s,redeem,bad,"wrong change amount");
  old=serialize(s);rejects([&]{e.apply(s,redeem,1);},"ordinary entry point cannot release native funds");check(serialize(s)==old,"unverified release atomic");
  e.apply_verified_native(s,redeem,1,release);check(s.native_locked==800000 && balance(s,alice.pub)==200000,"withdrawal debits exact owner balance and backing");
  rejected(e,s,redeem,release,"withdrawal replay");
  withdraw.nonce=6;withdraw.quantity=300000;withdraw.limit=0;redeem=alice.sign(withdraw);rejected(e,s,redeem,effect(redeem,0,300000,bob.pub),"withdrawal cannot spend someone else's pool or treasury balance");
  operation burn;burn.type=kind::burn;burn.chain=chain;burn.deadline=100;burn.nonce=6;burn.asset=native_asset;burn.quantity=1;
  old=serialize(s);rejects([&]{e.apply(s,alice.sign(burn),1);},"native burn cannot bypass release accounting");check(serialize(s)==old,"native burn rejection atomic");
  operation remove;remove.type=kind::liquidity_remove;remove.chain=chain;remove.deadline=100;remove.nonce=6;remove.asset=native_asset;remove.other=token;remove.quantity=500000;remove.minimum=1;remove.limit=1;
  e.apply(s,alice.sign(remove),1);check(s.pools.empty() && balance(s,alice.pub)==799950,"liquidity withdrawal preserves native backing");
  withdraw.nonce=7;withdraw.quantity=799950;withdraw.limit=0;redeem=alice.sign(withdraw);e.apply_verified_native(s,redeem,1,effect(redeem,0,799950,bob.pub));
  check(s.native_locked==50 && balance(s,treasury.pub)==50,"treasury remains backed after owner exit");
  withdraw.nonce=1;withdraw.quantity=50;auto treasury_exit=treasury.sign(withdraw);e.apply_verified_native(s,treasury_exit,1,effect(treasury_exit,0,50,bob.pub));
  check(s.native_locked==0 && balance(s,treasury.pub)==0,"full native redemption");check(deserialize(serialize(s))==s && serialize(s)[7]==2,"empty backed asset retains state version");
  // Real LMDB transactions exercise mixed block application and persisted undo.
  database db;const id first=key(501),second=key(502);bytes committed;
  db.write([&](MDB_txn* txn){lmdb_store store(txn,e);auto result=store.append_verified(txn,1,{},first,{{deposit,proof}});check(result.next.native_locked==1000000,"native deposit block append");committed=serialize(result.next);});
  db.write([&](MDB_txn* txn){lmdb_store store(txn,e);check(serialize(store.load(txn))==committed,"backed state persists across transactions");},false);
  rejects([&]{db.write([&](MDB_txn* txn){lmdb_store store(txn,e);store.append(txn,2,first,second,{deposit});});},"legacy append cannot credit native deposit");
  auto bob_dep=bob.sign(dep);auto bob_proof=effect(bob_dep,1000000,0);bob_proof.transaction=proof.transaction;
  rejects([&]{e.apply_verified_block(state{},1,{},first,{{deposit,proof},{bob_dep,bob_proof}});},"same native transaction cannot credit two signed owners in one block");
  auto next_dep=dep;next_dep.nonce=2;auto next=alice.sign(next_dep);auto next_proof=effect(next,1000000,0);next_proof.intent[0]^=1;
  rejects([&]{db.write([&](MDB_txn* txn){lmdb_store store(txn,e);store.append_verified(txn,2,first,second,{{next,next_proof}});});},"invalid native effect cannot commit a block");
  db.write([&](MDB_txn* txn){lmdb_store store(txn,e);check(serialize(store.load(txn))==committed,"failed native effect leaves current state and undo unchanged");},false);
  rejects([&]{db.write([&](MDB_txn* txn){lmdb_store wrong(txn,disabled);});},"native policy cannot reopen with legacy settings");
  db.write([&](MDB_txn* txn){lmdb_store store(txn,e);store.append_verified(txn,2,first,second,{{issue,std::nullopt}});},false);
  db.write([&](MDB_txn* txn){lmdb_store store(txn,e);check(serialize(store.load(txn))==committed,"aborted candidate leaves backing and nonce unchanged");},false);
  db.write([&](MDB_txn* txn){lmdb_store store(txn,e);store.append_verified(txn,2,first,second,{{issue,std::nullopt}});});
  db.write([&](MDB_txn* txn){lmdb_store store(txn,e);store.pop(txn,second);check(serialize(store.load(txn))==committed,"persisted token block undo restores native ledger");store.pop(txn,first);check(store.load(txn)==state{},"native deposit undo restores exact empty legacy state");});
  db.write([&](MDB_txn* txn){lmdb_store store(txn,e);store.append_verified(txn,1,{},first,{{deposit,proof}});check(serialize(store.load(txn))==committed,"native deposit deterministic replay");});
  db.reopen();
  db.write([&](MDB_txn* txn){lmdb_store store(txn,e);check(serialize(store.load(txn))==committed,"environment reopen restores exact backed state");},false);
  withdraw.nonce=2;withdraw.quantity=250000;withdraw.limit=100000;
  auto persisted_exit=alice.sign(withdraw);auto persisted_release=effect(persisted_exit,100000,350000,bob.pub);
  db.write([&](MDB_txn* txn){lmdb_store store(txn,e);auto result=store.append_verified(txn,2,first,second,{{persisted_exit,persisted_release}});check(result.next.native_locked==750000 && balance(result.next,alice.pub)==750000,"withdrawal block persists balance and backing debit together");});
  db.reopen();
  db.write([&](MDB_txn* txn){lmdb_store store(txn,e);check(store.load(txn).native_locked==750000,"withdrawal survives environment restart");store.pop(txn,second);check(serialize(store.load(txn))==committed,"persisted withdrawal undo restores original nonce, balance and backing");});
  std::cout<<checks<<" checks passed: signed native deposit/withdrawal ledger, native/token pools and fees, owner balances, effect binding, versioned state/policy, LMDB abort/undo/replay\n";
  std::cout<<"Internal effect fixtures only. Native transaction proof producer, output routing, withdrawal consensus and daemon activation remain disconnected.\n";
}catch(const std::exception& error){std::cerr<<"FAIL after "<<checks<<" checks: "<<error.what()<<'\n';return 1;}}
