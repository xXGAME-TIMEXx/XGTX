#pragma once
#include "engine.hpp"
namespace xgtx_exchange {
// Candidate consensus schedule; enabled only in the dedicated candidate build.
inline constexpr amount public_activation_index=700;
inline constexpr amount public_activation_count=public_activation_index+1;
inline constexpr std::uint8_t public_protocol_version=16;
inline constexpr id public_genesis={0x6c,0xc8,0x6a,0x50,0xec,0xa6,0xfe,0xbe,0x79,0x5d,0xc2,0x54,0x90,0xd0,0x2f,0x29,0xfe,0x8a,0x55,0x90,0xe9,0x3d,0xaf,0x03,0xd1,0xe0,0xc3,0x68,0x19,0x1d,0x74,0x35};
#ifdef XGTX_PUBLIC_EXCHANGE_CANDIDATE
inline constexpr bool public_candidate_compiled=true;
#else
inline constexpr bool public_candidate_compiled=false;
#endif
inline bool active_at_ordinal(const config& c,amount ordinal){return ordinal>=c.activation_height;}
inline bool active_for_next_block(const config& c,amount current_count){
  return current_count<std::numeric_limits<amount>::max() && active_at_ordinal(c,current_count+1);
}
}
