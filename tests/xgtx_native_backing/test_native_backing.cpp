#include "../../src/xgtx_exchange/native_backing.hpp"
#include <iostream>
#include <limits>
using namespace xgtx_exchange;
namespace b=xgtx_exchange::backing;
unsigned checks=0;
void check(bool value,const char* label){++checks;if(!value)throw std::runtime_error(label);}
id key(std::uint64_t n){id value{};for(unsigned i=0;i<8;++i)value[i]=n>>(8*i);return value;}
void rejects(b::reserve_state& s,const b::effect& e,amount before,amount after,const char* label){
  auto old=s;bool failed=false;try{b::apply(s,e,before,after);}catch(const std::invalid_argument&){failed=true;}
  check(failed,label);check(s==old,"rejection changed reserve state");
}
int main(){try{
  b::reserve_state s;s.chain=key(1);b::invariants(s,0);
  b::effect deposit{key(1),key(2),{},{{0,40},{1,60}}};
  rejects(s,deposit,0,101,"fabricated deposit credit");
  rejects(s,deposit,0,99,"miner fee incorrectly counted as reserve");
  auto bad=deposit;bad.chain=key(8);rejects(s,bad,0,100,"cross-chain deposit");
  bad=deposit;bad.transaction={};rejects(s,bad,0,100,"zero transaction");
  bad=deposit;bad.outputs[1].first=0;rejects(s,bad,0,100,"duplicate output");
  bad=deposit;bad.outputs[0].second=0;rejects(s,bad,0,60,"zero output");
  bad=deposit;bad.outputs[0].first=16;rejects(s,bad,0,100,"output index cap");
  b::apply(s,deposit,0,100);check(s.locked==100 && s.unspent.size()==2,"deposit");
  rejects(s,deposit,100,200,"transaction replay");
  b::effect withdrawal{key(1),key(3),{{key(2),1}},{{0,35}}};
  rejects(s,withdrawal,100,74,"release too much");
  rejects(s,withdrawal,100,76,"release too little");
  bad=withdrawal;bad.inputs.push_back(bad.inputs[0]);rejects(s,bad,100,15,"duplicate input");
  bad=withdrawal;bad.inputs[0].transaction=key(99);rejects(s,bad,100,75,"unknown reserve input");
  b::apply(s,withdrawal,100,75);check(s.locked==75 && s.spent.size()==1,"withdrawal with change");
  bad=withdrawal;bad.transaction=key(4);rejects(s,bad,75,50,"spent reserve input");
  auto snapshot=s;
  b::effect exit{key(1),key(5),{{key(2),0},{key(3),0}}, {}};
  b::apply(s,exit,75,0);check(s.locked==0 && s.unspent.empty(),"full redemption");
  s=snapshot;b::apply(s,exit,75,0);check(s.locked==0,"deterministic replay after snapshot restore");
  auto corrupt=s;corrupt.locked=1;rejects(corrupt,{key(1),key(6),{},{{0,1}}},1,2,"corrupt backing state");
  b::reserve_state huge;huge.chain=key(1);
  auto max=std::numeric_limits<amount>::max();
  b::apply(huge,{key(1),key(7),{},{{0,max}}},0,max);
  rejects(huge,{key(1),key(8),{},{{0,1}}},max,0,"reserve sum overflow");
  b::effect too_many{key(1),key(9),{}, {}};for(unsigned j=0;j<17;++j)too_many.outputs.push_back({j,1});
  rejects(s,too_many,0,17,"output count cap");
  rejects(s,{key(1),key(10),{},{}},0,0,"empty effect");
  // Repeated independent deposit/change/redemption and rollback cycles.
  for(unsigned i=0;i<1000;++i){
    b::reserve_state x;x.chain=key(1);amount n=100+i;
    auto tx=key(100+3*i),tx2=key(101+3*i),tx3=key(102+3*i);
    b::apply(x,{key(1),tx,{},{{0,n}}},0,n);auto initial=x;
    b::apply(x,{key(1),tx2,{{tx,0}},{{0,n-1}}},n,n-1);
    check(x.locked==n-1,"change cycle");auto after=x;
    rejects(x,{key(1),tx3,{{tx,0}},{}},n-1,0,"cycle double spend");
    x=initial;b::apply(x,{key(1),tx2,{{tx,0}},{{0,n-1}}},n,n-1);
    check(x==after,"cycle replay identical");
    b::apply(x,{key(1),tx3,{{tx2,0}},{}},n-1,0);
    check(x.locked==0 && x.unspent.empty(),"cycle fully redeemed");
  }
  std::cout<<checks<<" checks passed: reserve conservation, duplicate/replay rejection, net redemption/change, overflow, atomic failure and snapshot replay\n";
  std::cout<<"Accounting model only. No native proof verification, protected reserve outputs, ledger integration, persistence or daemon activation.\n";
}catch(const std::exception& e){std::cerr<<"FAIL after "<<checks<<" checks: "<<e.what()<<'\n';return 1;}}
