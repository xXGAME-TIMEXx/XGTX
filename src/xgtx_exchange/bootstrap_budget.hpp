#pragma once
#include "engine.hpp"
namespace xgtx_exchange {
// An explicit bootstrap budget caps the entire ordinary native input spend,
// including its fee. Never silently deposit a selected input above that cap.
inline amount bootstrap_credit(amount selected,amount budget,amount fee,amount max_fee){
  if(!selected || selected>budget)throw std::invalid_argument("Selected bootstrap inputs exceed spending cap; choose a larger cap or consolidate a smaller input first");
  if(!fee || fee>max_fee || fee>=selected || selected-fee<16)throw std::invalid_argument("Bootstrap fee exceeds ceiling or leaves fewer than sixteen reserve units");
  return selected-fee;
}
}
