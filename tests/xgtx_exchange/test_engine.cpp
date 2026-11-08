#include <algorithm>
#include "../../src/xgtx_exchange/engine.hpp"
#include "../../src/xgtx_exchange/lmdb_store.hpp"
#ifdef XGTX_TEST_CRYPTONOTE
#include "test_native_crypto.hpp"
#else
#include "test_crypto.hpp"
#endif
#include <filesystem>
#include <iostream>
#include <random>
#include <limits>
#include <unistd.h>
using namespace xgtx_exchange;
using xgtx_test::wallet;
int checks=0;
void check(bool ok,const char* name){++checks;if(!ok)throw std::runtime_error(name);}
template<class F> void reject(F f,const char* name){bool failed=false;try{f();}catch(const std::exception&){failed=true;}check(failed,name);}
id named(std::uint8_t n){id a{};a.fill(n);return a;}
amount balance(const state& s,id user,id asset){auto i=s.balances.find({user,asset});return i==s.balances.end()?0:i->second;}
amount shares(const state& s,id user,id pid){auto i=s.lp.find({user,pid});return i==s.lp.end()?0:i->second;}
operation base(const engine& e,const state& s,const wallet& w,kind k){operation o;o.type=k;o.chain=e.settings().chain;auto it=s.nonces.find(w.pub);o.nonce=it==s.nonces.end()?1:it->second+1;o.deadline=100000;o.quantity=1;return o;}
receipt run(const engine& e,state& s,const wallet& w,operation o){return e.apply(s,w.sign(o),std::max<amount>(1,s.height+1));}
id create(const engine& e,state& s,const wallet& w,const char* name,amount quantity){auto o=base(e,s,w,kind::create);o.ticker=name;o.quantity=quantity;o.decimals=10;return run(e,s,w,o).asset;}
struct database {
  MDB_env* env=nullptr;std::filesystem::path path;
  database(){char name[]="/tmp/xgtx-exchange-XXXXXX";auto p=mkdtemp(name);if(!p)throw std::runtime_error("temp dir");path=p;open();}
  void open(){if(mdb_env_create(&env)||mdb_env_set_maxdbs(env,8)||mdb_env_set_mapsize(env,64*1024*1024)||mdb_env_open(env,path.c_str(),0,0600))throw std::runtime_error("LMDB open");}
  void reopen(){mdb_env_close(env);env=nullptr;open();}
  ~database(){if(env)mdb_env_close(env);std::filesystem::remove_all(path);}
  MDB_txn* begin(bool readonly=false){MDB_txn* t=nullptr;if(mdb_txn_begin(env,nullptr,readonly?MDB_RDONLY:0,&t))throw std::runtime_error("LMDB begin");return t;}
  void commit(MDB_txn* t){if(mdb_txn_commit(t))throw std::runtime_error("LMDB commit");}
};
int main(){try{
  wallet alice(1),bob(2),treasury(3),attacker(4);
  engine e({named(9),treasury.pub,1,1024},xgtx_test::hash,xgtx_test::verify);state s;
  auto a=create(e,s,alice,"GAME",1000000000),b=create(e,s,alice,"PLAY",1000000000);
  reject([&]{create(e,s,alice,"XGTX",100);},"native ticker reserved");
  check(a!=b,"unique asset ids");check(s.assets.at(a).supply==1000000000,"issuance supply");
  auto transfer=base(e,s,alice,kind::transfer);transfer.asset=a;transfer.recipient=bob.pub;transfer.quantity=1000000;run(e,s,alice,transfer);
  check(balance(s,bob.pub,a)==1000000,"signed transfer");
  auto burn=base(e,s,bob,kind::burn);burn.asset=a;burn.quantity=1;run(e,s,bob,burn);check(s.assets.at(a).supply==999999999,"burn supply");
  auto before=s;
  auto replay=alice.sign(transfer);reject([&]{e.apply(s,replay,1);},"replay rejected");check(s==before,"replay atomic");
  auto wrong=base(e,s,alice,kind::transfer);wrong.asset=a;wrong.recipient=bob.pub;wrong.quantity=1;wrong.chain=named(8);reject([&]{run(e,s,alice,wrong);},"wrong chain");
  wrong.chain=e.settings().chain;wrong.deadline=0;reject([&]{run(e,s,alice,wrong);},"expiry");
  wrong.deadline=1000;auto forged=alice.sign(wrong);forged.sig[0]^=1;reject([&]{e.apply(s,forged,1);},"forged signature");
  forged=alice.sign(wrong);forged.op.quantity++;reject([&]{e.apply(s,forged,1);},"unsigned mutation");
  auto stolen=attacker.sign(wrong);stolen.op.sender=alice.pub;reject([&]{e.apply(s,stolen,1);},"wrong key");
  wrong.quantity=1000000001;reject([&]{run(e,s,alice,wrong);},"overdraft");
  check(s==before,"rejection leaves state unchanged");
  wrong.quantity=1;auto normal=alice.sign(wrong);reject([&]{e.apply(s,normal,0);},"activation gate");
  auto native=wrong;native.asset=native_asset;reject([&]{run(e,s,alice,native);},"native bridge disabled");
  auto bytes=encode(normal.op);check(encode(decode(bytes))==bytes,"operation roundtrip");check(bytes.size()==212,"fixed operation size");
  auto signedbytes=encode_envelope(normal);check(encode_envelope(decode_envelope(signedbytes))==signedbytes,"signed envelope roundtrip");
  for(std::size_t n=0;n<signedbytes.size();++n){auto t=signedbytes;t.resize(n);reject([&]{decode_envelope(t);},"truncated signed envelope");}
  for(std::size_t n=0;n<bytes.size();++n){auto t=bytes;t.resize(n);reject([&]{decode(t);},"truncated operation");}
  auto extra=bytes;extra.push_back(0);reject([&]{decode(extra);},"trailing operation byte");
  for(std::size_t n=0;n<9;++n){auto t=bytes;t[n]^=0xff;reject([&]{decode(t);},"unknown operation domain/version");}
  auto p=base(e,s,alice,kind::pool_create);p.asset=a;p.other=b;p.quantity=1000000;p.limit=1000000;
  auto pr=run(e,s,alice,p);auto pid=pr.pool_id;check(pr.shares==1000000,"initial geometric LP supply");
  before=s;p=base(e,s,alice,kind::pool_create);p.asset=b;p.other=a;p.quantity=100;p.limit=100;reject([&]{run(e,s,alice,p);},"reverse duplicate pool");check(s==before,"pool rejection atomic");
  auto sw=base(e,s,alice,kind::swap);sw.asset=a;sw.other=b;sw.quantity=10000;sw.minimum=1;auto quote=run(e,s,alice,sw);
  check(quote.lp_fee==25 && quote.protocol_fee==5,"25/5 bps fees");check(quote.output==9871,"constant product quote");check(balance(s,treasury.pub,a)==5,"treasury input asset credit");
  check(balance(s,treasury.pub,b)==0,"no implicit fee conversion");
  before=s;sw=base(e,s,alice,kind::swap);sw.asset=b;sw.other=a;sw.quantity=10000;sw.minimum=std::numeric_limits<amount>::max();reject([&]{run(e,s,alice,sw);},"swap slippage");check(s==before,"swap failure atomic");
  sw.minimum=1;auto rev=run(e,s,alice,sw);check(rev.protocol_fee==5 && balance(s,treasury.pub,b)==5,"reverse fees");
  auto deposit=base(e,s,alice,kind::liquidity_add);deposit.asset=a;deposit.other=b;deposit.quantity=10000;deposit.limit=10000;deposit.minimum=1;auto d=run(e,s,alice,deposit);
  check(d.shares && d.used_a<=10000 && d.used_b<=10000,"proportional deposit");
  before=s;deposit=base(e,s,alice,kind::liquidity_add);deposit.asset=a;deposit.other=b;deposit.quantity=100;deposit.limit=100;deposit.minimum=1000000;reject([&]{run(e,s,alice,deposit);},"LP mint slippage");check(s==before,"deposit failure atomic");
  auto withdraw=base(e,s,alice,kind::liquidity_remove);withdraw.asset=a;withdraw.other=b;withdraw.quantity=d.shares;withdraw.minimum=1;withdraw.limit=1;auto wr=run(e,s,alice,withdraw);check(wr.used_a && wr.used_b,"withdrawal");
  withdraw=base(e,s,bob,kind::liquidity_remove);withdraw.asset=a;withdraw.other=b;withdraw.quantity=1;withdraw.minimum=1;withdraw.limit=1;before=s;reject([&]{run(e,s,bob,withdraw);},"LP ownership");check(s==before,"withdraw rejection atomic");
  withdraw=base(e,s,alice,kind::liquidity_remove);withdraw.asset=a;withdraw.other=b;withdraw.quantity=1;withdraw.minimum=std::numeric_limits<amount>::max();withdraw.limit=1;before=s;reject([&]{run(e,s,alice,withdraw);},"withdrawal slippage");check(s==before,"withdrawal slippage atomic");
  std::mt19937_64 rng(17);
  for(unsigned n=0;n<500;++n){auto o=base(e,s,alice,kind::swap);bool direction=rng()%2;o.asset=direction?a:b;o.other=direction?b:a;o.quantity=1000+rng()%10000;o.minimum=1;auto old=s;auto quoted=e.quote_swap(s,o.asset,o.other,o.quantity);check(s==old,"quote leaves state unchanged");auto rr=run(e,s,alice,o);check(quoted.output==rr.output && quoted.lp_fee==rr.lp_fee && quoted.protocol_fee==rr.protocol_fee && quoted.pool_id==rr.pool_id,"quote matches confirmed swap execution");check(rr.lp_fee==o.quantity*25/10000 && rr.protocol_fee==o.quantity*5/10000,"random fee arithmetic");e.invariants(s);check(s.assets.at(a).supply==old.assets.at(a).supply && s.assets.at(b).supply==old.assets.at(b).supply,"random supply conservation");}
  auto snapshot=serialize(s);check(deserialize(snapshot)==s,"state roundtrip");
  auto reordered=snapshot;std::swap_ranges(reordered.begin()+56,reordered.begin()+134,reordered.begin()+134);reject([&]{deserialize(reordered);},"noncanonical state order");
  auto duplicated=snapshot;std::copy_n(duplicated.begin()+56,78,duplicated.begin()+134);reject([&]{deserialize(duplicated);},"duplicate state asset");
  for(std::size_t n=0;n<snapshot.size();++n){auto t=snapshot;t.resize(n);reject([&]{deserialize(t);},"truncated state");}
  extra=snapshot;extra.push_back(0);reject([&]{deserialize(extra);},"trailing state byte");
  auto inflated=s;inflated.balances[{alice.pub,a}]++;reject([&]{e.invariants(inflated);},"inflation invariant");
  inflated=s;inflated.lp[{alice.pub,pid}]++;reject([&]{e.invariants(inflated);},"LP inflation invariant");
  before=s;withdraw=base(e,s,alice,kind::liquidity_remove);withdraw.asset=a;withdraw.other=b;withdraw.quantity=shares(s,alice.pub,pid);withdraw.minimum=1;withdraw.limit=1;run(e,s,alice,withdraw);check(s.pools.empty() && s.lp.empty(),"final withdrawal removes empty pool");e.invariants(s);
  auto maximal=create(e,s,bob,"MAX",std::numeric_limits<amount>::max());check(s.assets.at(maximal).supply==std::numeric_limits<amount>::max(),"maximum supply");
  auto maximal2=create(e,s,bob,"MAX2",std::numeric_limits<amount>::max());
  auto giant=base(e,s,bob,kind::pool_create);giant.asset=maximal;giant.other=maximal2;giant.quantity=std::numeric_limits<amount>::max();giant.limit=giant.quantity;
  auto giantreceipt=run(e,s,bob,giant);check(giantreceipt.shares==std::numeric_limits<amount>::max(),"128-bit initial LP calculation");
  auto all=base(e,s,bob,kind::liquidity_remove);all.asset=maximal;all.other=maximal2;all.quantity=giant.quantity;all.minimum=1;all.limit=1;run(e,s,bob,all);check(balance(s,bob.pub,maximal)==giant.quantity && balance(s,bob.pub,maximal2)==giant.quantity,"maximum LP withdrawal");
  operation c=base(e,state{},alice,kind::create);c.ticker="GAME";c.quantity=1000000;auto env=alice.sign(c);
  state zero;auto block=e.apply_block(zero,1,id{},named(1),{env});check(block.next.height==1 && block.next.tip==named(1),"block linkage");
  reject([&]{e.apply_block(zero,2,id{},named(1),{env});},"height gap");reject([&]{e.apply_block(zero,1,named(8),named(1),{env});},"wrong parent");reject([&]{e.apply_block(zero,1,id{},named(1),{env,env});},"invalid second operation aborts block");check(zero.assets.empty(),"original block atomic");
  reject([&]{e.apply_block(zero,1,id{},named(1),std::vector<envelope>(1025,env));},"operation resource cap");
  auto future=e.apply_block(block.next,2,named(1),named(2),{});check(future.next.assets.size()==1,"empty block allowed");
  // Two nodes execute the same signed block sequence independently.
  auto other=e.apply_block(zero,1,id{},named(1),{env});auto other2=e.apply_block(other.next,2,named(1),named(2),{});check(other2.next==future.next,"two-node deterministic replay");
  database db;auto txn=db.begin();lmdb_store store(txn,e);db.commit(txn);
  txn=db.begin();auto persisted=store.append(txn,1,id{},named(1),{env});db.commit(txn);
  txn=db.begin(true);check(store.load(txn)==persisted.next,"persisted block");mdb_txn_abort(txn);
  txn=db.begin();store.append(txn,2,named(1),named(2),{});mdb_txn_abort(txn);
  txn=db.begin(true);check(store.load(txn)==persisted.next,"aborted database transaction leaves state unchanged");mdb_txn_abort(txn);
  // Native block marker and exchange update share one transaction: both abort.
  txn=db.begin();MDB_dbi marker;check(mdb_dbi_open(txn,"native_test_marker",MDB_CREATE,&marker)==0,"native marker db");db.commit(txn);
  txn=db.begin();
  std::string mk="height",mv="2";MDB_val key{mk.size(),mk.data()},value{mv.size(),mv.data()};check(mdb_put(txn,marker,&key,&value,0)==0,"native marker put");store.append(txn,2,named(1),named(2),{});mdb_txn_abort(txn);
  txn=db.begin(true);check(store.load(txn).height==1,"shared transaction rollback");MDB_val missing{};check(mdb_get(txn,marker,&key,&missing)==MDB_NOTFOUND,"native marker also rolled back");mdb_txn_abort(txn);
  txn=db.begin();store.append(txn,2,named(1),named(2),{});db.commit(txn);
  db.reopen();txn=db.begin();lmdb_store reopened(txn,e);check(reopened.load(txn)==future.next,"restart recovery");db.commit(txn);
  txn=db.begin();reject([&]{reopened.pop(txn,named(99));},"wrong reorg tip");mdb_txn_abort(txn);
  txn=db.begin();auto rolled=reopened.pop(txn,named(2));check(rolled==persisted.next,"block rollback");db.commit(txn);
  txn=db.begin();auto genesis=reopened.pop(txn,named(1));check(genesis==zero,"issuance rollback");db.commit(txn);
  txn=db.begin();auto replayed=reopened.append(txn,1,id{},named(11),{env});auto expected=block.next;expected.tip=named(11);check(replayed.next==expected,"nonce reusable on replacement branch");db.commit(txn);
  auto wrongconfig=e.settings();wrongconfig.treasury=bob.pub;engine changed(wrongconfig,xgtx_test::hash,xgtx_test::verify);
  txn=db.begin();reject([&]{lmdb_store bad(txn,changed);},"fee recipient configuration guard");mdb_txn_abort(txn);
  std::cout<<checks<<" checks passed: signed assets, AMM, LP, fees, conservation, malformed input, LMDB abort/restart/reorg, deterministic replay\n";
  return 0;
}catch(const std::exception& ex){std::cerr<<"FAIL after "<<checks<<" checks: "<<ex.what()<<"\n";return 1;}}
