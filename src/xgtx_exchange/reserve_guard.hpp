#pragma once
#include "serialization/string.h"
#include "engine.hpp"
#include "cryptonote_basic/cryptonote_basic.h"
#include <limits>
namespace xgtx_exchange::reserve_policy {
inline constexpr amount legacy_bucket=std::numeric_limits<amount>::max();
inline constexpr amount bucket=native_reserve_bucket;
inline bool has_reserved_input(const cryptonote::transaction_prefix& tx){
  for(const auto& input:tx.vin)
    if(input.type()==typeid(cryptonote::txin_to_key) && (boost::get<cryptonote::txin_to_key>(input).amount==bucket || boost::get<cryptonote::txin_to_key>(input).amount==legacy_bucket))return true;
  return false;
}
inline void reject_unreleased_inputs(const cryptonote::transaction_prefix& tx){
  if(has_reserved_input(tx))throw std::invalid_argument("native reserve release not implemented");
}
}
