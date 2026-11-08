#pragma once
#include "native_carrier.hpp"
#include "ringct/rctSigs.h"

namespace xgtx_exchange {
// Generate must invoke its final-message callback exactly once after constructing
// the final RingCT base and before generating native ring signatures. Work is on
// a candidate copy; caller's transaction remains unchanged if any step throws.
template<class Generate>
cryptonote::transaction sign_carrier_transaction(const cryptonote::transaction& original,
    const envelope& intent,const crypto::public_key& owner,const crypto::secret_key& secret,
    Generate generate){
  cryptonote::transaction candidate=original;
  std::size_t calls=0;
  auto result=generate([&](const rct::rctSig& base)->rct::key{
    carrier_require(++calls==1,"native signing callback repeated");
    candidate.rct_signatures=base;
    seal_native_carrier(candidate,intent,owner,secret);
    return rct::hash2rct(cryptonote::get_transaction_prefix_hash(candidate));
  });
  carrier_require(calls==1,"native signing callback missing");
  candidate.rct_signatures=std::move(result);candidate.invalidate_hashes();
  verify_native_carrier(candidate,intent.op.chain);
  carrier_require(candidate.rct_signatures.message==rct::hash2rct(cryptonote::get_transaction_prefix_hash(candidate)),"native prefix/signature message mismatch");
  carrier_require(rct::verRctSimple(candidate.rct_signatures),"generated native RingCT proofs invalid");
  return candidate;
}
}
