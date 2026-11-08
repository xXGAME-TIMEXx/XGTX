#pragma once
#include "engine.hpp"
#include "crypto/crypto.h"
#include "crypto/hash.h"
#include <cstring>

namespace xgtx_exchange {
// Source adapter to the chain's existing primitives. This header still requires
// a full daemon build; standalone tests use the separate Ed25519 test adapter.
inline id cryptonote_hash(const bytes& b){
  crypto::hash h=crypto::cn_fast_hash(b.data(),b.size());id out{};
  static_assert(sizeof(h)==32);std::memcpy(out.data(),&h,32);return out;
}
inline bool cryptonote_verify(const id& sender,const bytes& message,const signature& sig){
  crypto::public_key pub{};crypto::signature s{};
  static_assert(sizeof(pub)==32 && sizeof(s)==64);
  std::memcpy(&pub,sender.data(),32);std::memcpy(&s,sig.data(),64);
  if(!crypto::check_key(pub))return false;
  auto hash=crypto::cn_fast_hash(message.data(),message.size());return crypto::check_signature(hash,pub,s);
}
inline envelope cryptonote_sign(operation op,const crypto::public_key& pub,const crypto::secret_key& secret){
  crypto::public_key derived{};
  if(!crypto::secret_key_to_public_key(secret,derived) || std::memcmp(&derived,&pub,sizeof(pub)))throw std::invalid_argument("signing key mismatch");
  std::memcpy(op.sender.data(),&pub,32);envelope e{op,{}};auto b=signing_message(op);
  auto h=crypto::cn_fast_hash(b.data(),b.size());crypto::signature sig{};crypto::generate_signature(h,pub,secret,sig);std::memcpy(e.sig.data(),&sig,64);return e;
}
}
