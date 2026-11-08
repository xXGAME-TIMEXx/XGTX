#include "../../src/xgtx_exchange/activation_policy.hpp"
#include <iostream>
int main(){using namespace xgtx_exchange;unsigned checks=0;auto check=[&](bool x){++checks;if(!x)throw std::runtime_error("activation boundary");};config c;c.activation_height=public_activation_count;
 struct boundary{amount height;bool ordinal,next;};
 for(auto row:{boundary{0,false,false},boundary{642,false,false},boundary{699,false,false},boundary{700,false,true},boundary{701,true,true},boundary{702,true,true}}){check(active_at_ordinal(c,row.height)==row.ordinal);check(active_for_next_block(c,row.height)==row.next);}
 check(public_protocol_version==16);check(public_genesis!=id{});check(!active_for_next_block(c,std::numeric_limits<amount>::max()));c.activation_height=0;check(active_for_next_block(c,0));
#ifdef XGTX_PUBLIC_EXCHANGE_CANDIDATE
 check(public_candidate_compiled);
#else
 check(!public_candidate_compiled);
#endif
 std::cout<<checks<<" checks passed: activation ordinal and candidate compile gate\n";
}
