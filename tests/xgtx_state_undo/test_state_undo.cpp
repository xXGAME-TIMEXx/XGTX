#include "../../src/xgtx_exchange/lmdb_store.hpp"
#ifdef XGTX_TEST_CRYPTONOTE
#include "../xgtx_exchange/test_native_crypto.hpp"
#else
#include "../xgtx_exchange/test_crypto.hpp"
#endif
#include <filesystem>
#include <iostream>
#include <random>
#include <unistd.h>
using namespace xgtx_exchange;
unsigned checks=0;
void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
template<class F>void reject(F f,const char* why){bool failed=false;try{f();}catch(const std::exception&){failed=true;}check(failed,why);}
id key(amount n){id k{};for(unsigned i=0;i<8;++i)k[i]=static_cast<std::uint8_t>(n>>(i*8));return k;}
struct database{
 MDB_env* env=nullptr;std::filesystem::path path;
 database(){char name[]="/tmp/xgtx-undo-XXXXXX";auto p=mkdtemp(name);if(!p)throw std::runtime_error("mkdtemp");path=p;open();}
 void open(){if(mdb_env_create(&env)||mdb_env_set_maxdbs(env,8)||mdb_env_set_mapsize(env,128*1024*1024)||mdb_env_open(env,path.c_str(),0,0600))throw std::runtime_error("LMDB open");}
 void reopen(){mdb_env_close(env);env=nullptr;open();}
 ~database(){if(env)mdb_env_close(env);std::filesystem::remove_all(path);}
 MDB_txn* begin(bool ro=false){MDB_txn* t=nullptr;if(mdb_txn_begin(env,nullptr,ro?MDB_RDONLY:0,&t))throw std::runtime_error("LMDB begin");return t;}
 void commit(MDB_txn* t){if(mdb_txn_commit(t))throw std::runtime_error("LMDB commit");}
};
std::string undo_key(amount n){std::string s="undo/";for(unsigned i=0;i<8;++i)s.push_back(static_cast<char>(n>>(i*8)));return s;}
void put(MDB_txn* t,const std::string& k,const bytes& b){MDB_dbi db;if(mdb_dbi_open(t,"xgtx_exchange_state_v1",0,&db))throw std::runtime_error("table");MDB_val a{k.size(),const_cast<char*>(k.data())},v{b.size(),const_cast<std::uint8_t*>(b.data())};if(mdb_put(t,db,&a,&v,0))throw std::runtime_error("put");}
bytes get(MDB_txn* t,const std::string& k){MDB_dbi db;if(mdb_dbi_open(t,"xgtx_exchange_state_v1",0,&db))throw std::runtime_error("table");MDB_val a{k.size(),const_cast<char*>(k.data())},v{};int rc=mdb_get(t,db,&a,&v);if(rc==MDB_NOTFOUND)return {};if(rc)throw std::runtime_error("get");auto p=static_cast<const std::uint8_t*>(v.mv_data);return bytes(p,p+v.mv_size);}
int main(){try{
 engine e({key(999),key(998),1,1024},xgtx_test::hash,xgtx_test::verify);
 // Codec covers all map variants independently of economic invariants. Native
 // engine/store suites separately verify valid economic transitions.
 std::mt19937_64 rng(310031);
 for(unsigned trial=0;trial<250;++trial){
  state before;before.height=trial;before.tip=key(trial+99);before.native_locked=rng();
  for(amount i=1;i<25;++i){auto k=key(i);before.assets[k]={key(i+1),rng(),2,"AA"};before.balances[{k,key(i+1)}]=rng();before.lp[{k,key(i+2)}]=rng();before.nonces[k]=rng();before.pools[k]={key(i+1),key(i+2),rng(),rng(),rng()};}
  auto after=before;after.height++;after.tip=key(trial+100);after.native_locked=rng();
  for(amount i=1;i<30;++i){auto k=key(i);switch(rng()%3){case 0:after.assets.erase(k);after.balances.erase({k,key(i+1)});after.lp.erase({k,key(i+2)});after.nonces.erase(k);after.pools.erase(k);break;case 1:after.assets[k]={key(i+9),rng(),4,"TEST"};after.balances[{k,key(i+1)}]=rng();after.lp[{k,key(i+2)}]=rng();after.nonces[k]=rng();after.pools[k]={key(i+4),key(i+6),rng(),rng(),rng()};break;default:break;}}
  auto journal=encode_state_undo(before,after,e);check(restore_state_undo(after,journal,e)==before,"random all-map reverse roundtrip");
  auto wrong=after;wrong.nonces[key(1000)]=1;reject([&]{restore_state_undo(wrong,journal,e);},"wrong economic current state rejected");
  auto corrupt=journal;corrupt[8]^=1;reject([&]{restore_state_undo(after,corrupt,e);},"wrong prior digest rejected");
  corrupt=journal;corrupt.push_back(0);reject([&]{restore_state_undo(after,corrupt,e);},"trailing bytes rejected");
  for(auto cut:{std::size_t(0),std::size_t(7),journal.size()/2,journal.size()-1}){corrupt=bytes(journal.begin(),journal.begin()+cut);reject([&]{restore_state_undo(after,corrupt,e);},"truncated record rejected");}
 }
 state big;big.height=1;big.tip=key(1);
 for(amount i=1;i<=4000;++i){big.assets[key(i)]={key(9000),100,2,"TEST"};big.balances[{key(i),key(i)}]=100;}
 big.nonces[key(9000)]=4000;e.invariants(big);auto after=big;after.height=2;after.tip=key(2);
 auto idle=encode_state_undo(big,after,e);check(idle.size()==160,"idle journal constant 160 bytes");check(serialize(big).size()>idle.size()*1000,"large ledger empty-block storage reduction");
 after.balances[{key(1),key(1)}]=99;after.balances[{key(2),key(1)}]=1;
 auto small=encode_state_undo(big,after,e);check(small.size()<400,"two-balance journal bounded");check(restore_state_undo(after,small,e)==big,"large ledger targeted reverse");
 // Strict map encoding: mutate structurally valid journals independently of
 // digest rejection to reach marker/order/count validation.
 auto malformed=small;malformed[200]=2;reject([&]{restore_state_undo(after,malformed,e);},"bad presence rejected");
 malformed=small;std::copy_n(malformed.begin()+136,64,malformed.begin()+209);reject([&]{restore_state_undo(after,malformed,e);},"duplicate map key rejected");
 malformed=small;malformed[201]=99;reject([&]{restore_state_undo(after,malformed,e);},"unchanged entry rejected");
 malformed=idle;for(unsigned i=0;i<8;++i)malformed[120+i]=255;reject([&]{restore_state_undo([&]{auto s=big;s.height=2;s.tip=key(2);return s;}(),malformed,e);},"excessive count rejected");
 database db;auto t=db.begin();lmdb_store init(t,e);auto policy=get(t,"policy");check(policy[8]==4,"new writer policy version");
 bytes legacy=policy;legacy[8]=legacy.back();legacy.resize(legacy.size()-4);put(t,"policy",legacy);
 state zero,one;one.height=1;one.tip=key(1);put(t,"current",serialize(one));bytes oldundo(one.tip.begin(),one.tip.end());auto snapshot=serialize(zero);oldundo.insert(oldundo.end(),snapshot.begin(),snapshot.end());put(t,undo_key(1),oldundo);db.commit(t);
 t=db.begin(true);{lmdb_store reader(t,e);check(reader.load(t)==one,"legacy read-only load");check(get(t,"policy")==legacy,"read-only open does not migrate");}mdb_txn_abort(t);
 t=db.begin();{lmdb_store store(t,e);store.append(t,2,one.tip,key(2),{});check(get(t,"policy")==policy,"migration staged with append");}mdb_txn_abort(t);
 t=db.begin(true);check(get(t,"policy")==legacy && get(t,undo_key(2)).empty(),"aborted migration and journal atomic");mdb_txn_abort(t);
 t=db.begin();{lmdb_store store(t,e);store.append(t,2,one.tip,key(2),{});}db.commit(t);db.reopen();
 t=db.begin();{lmdb_store store(t,e);check(get(t,"policy")==policy,"migration persisted restart");check(get(t,undo_key(2)).size()==192,"stored empty journal bounded");auto current=store.load(t);auto valid=get(t,undo_key(2));auto bad=valid;bad.back()^=1;put(t,undo_key(2),bad);reject([&]{store.pop(t,key(2));},"corrupt undo pop fails");check(store.load(t)==current,"failed pop leaves current unchanged");put(t,undo_key(2),valid);check(store.pop(t,key(2))==one,"compact pop after restart");check(store.pop(t,key(1))==zero,"legacy full snapshot pop below compact history");}db.commit(t);
 t=db.begin();{lmdb_store store(t,e);amount total=0;id tip{};for(amount i=1;i<=100;++i){store.append(t,i,tip,key(i),{});tip=key(i);total+=get(t,undo_key(i)).size();}check(total==19200,"100 empty blocks have fixed journal storage");for(amount i=100;i>0;--i)store.pop(t,key(i));check(store.load(t)==zero,"100-block rollback");}db.commit(t);
 database large_db;t=large_db.begin();{lmdb_store store(t,e);put(t,"current",serialize(big));store.append(t,2,big.tip,key(2),{});check(get(t,undo_key(2)).size()==192,"persisted large-ledger idle journal stays bounded");check(store.pop(t,key(2))==big,"large-ledger LMDB rollback");}large_db.commit(t);
 auto cfg=e.settings();cfg.native_backing=true;engine backed(cfg,xgtx_test::hash,xgtx_test::verify);database backed_db;t=backed_db.begin();{lmdb_store store(t,backed);auto modern=get(t,"policy");auto old=modern;old[8]=old.back();old.resize(old.size()-4);check(old[8]==3,"backed legacy v3 retained");put(t,"policy",old);lmdb_store migration(t,backed);migration.append(t,1,id{},key(1),{});check(get(t,"policy")==modern,"backed policy migrates atomically");auto wrong_cfg=cfg;wrong_cfg.treasury=key(777);engine wrong(wrong_cfg,xgtx_test::hash,xgtx_test::verify);reject([&]{lmdb_store mismatch(t,wrong);},"configuration mismatch still rejected");}backed_db.commit(t);
 std::cout<<checks<<" checks passed: compact reverse journals, all-map randomized replay, corruption guards, bounded empty blocks, legacy mixed history, atomic policy migration and restart\n";
 }catch(const std::exception& e){std::cerr<<"FAIL after "<<checks<<" checks: "<<e.what()<<"\n";return 1;}}
