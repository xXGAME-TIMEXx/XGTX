#include "token_format.hpp"
#include <iostream>
#include <limits>
using namespace xgtx_draft;
int checks=0;
void check(bool b){++checks;if(!b)throw std::runtime_error("test failed");}
template<class F> void reject(F f){bool failed=false;try{f();}catch(const std::invalid_argument&){failed=true;}check(failed);}
int main(){
  for(std::uint8_t k=1;k<=3;++k){
    operation o;o.kind=k;o.chain.fill(0x11);o.nonce.fill(0x22);
    o.amount=std::numeric_limits<std::uint64_t>::max();
    if(k==1){o.ticker="GAME";o.decimals=9;}else{o.asset.fill(0x33);if(k==2)o.recipient.fill(0x44);}
    auto b=encode(o);check(encode(decode(b))==b);
    check(b.size()==(k==1?87:k==2?145:113));
    check(b[k==1?73:105]==255);
    for(std::size_t n=0;n<b.size();++n){auto t=b;t.resize(n);reject([&]{decode(t);});}
    auto t=b;t.push_back(0);reject([&]{decode(t);});
    for(std::size_t i=0;i<8;++i){t=b;t[i]^=0xff;reject([&]{decode(t);});}
    t=b;t[8]=4;reject([&]{decode(t);});
    t=b;std::fill_n(t.begin()+(k==1?73:105),8,0);reject([&]{decode(t);});
    o.amount=0;reject([&]{encode(o);});
  }
  operation o;o.ticker="GA";auto b=encode(o);
  check(b[73]==1 && b[74]==0 && b[81]==0 && b[82]==2 && b[83]=='G' && b[84]=='A');
  b[81]=13;reject([&]{decode(b);});
  for(auto s:{"aB","A","ABCDEFGHIJKLM","A-","1A"}){o.ticker=s;reject([&]{encode(o);});}
  o.ticker="GAME";o.asset[0]=1;reject([&]{encode(o);});
  std::cout<<checks<<" checks passed\n";
}
