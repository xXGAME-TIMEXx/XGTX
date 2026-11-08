#include "../../src/xgtx_exchange/bootstrap_budget.hpp"
#include <iostream>
using namespace xgtx_exchange;
int main(){unsigned checks=0;auto check=[&](bool v){++checks;if(!v)throw std::runtime_error("bootstrap budget boundary");};auto reject=[&](amount s,amount b,amount f,amount m){bool failed=false;try{bootstrap_credit(s,b,f,m);}catch(const std::invalid_argument&){failed=true;}check(failed);};
 check(bootstrap_credit(100,100,1,1)==99);check(bootstrap_credit(100,101,5,6)==95);check(bootstrap_credit(17,17,1,1)==16);
 reject(0,100,1,1);reject(101,100,1,1);reject(100,100,0,1);reject(100,100,2,1);reject(100,100,100,100);reject(100,100,101,101);reject(16,16,1,1);
 const auto top=std::numeric_limits<amount>::max();check(bootstrap_credit(top,top,top-16,top)==16);reject(top,top-1,1,1);reject(top,top,top-15,top);
 std::cout<<checks<<" bootstrap budget boundary checks passed\n";
}
