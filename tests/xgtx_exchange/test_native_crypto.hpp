#pragma once
#include "../../src/xgtx_exchange/cryptonote_crypto.hpp"
#include <cstring>

// Ephemeral test wallets only. No user wallet is opened or imported.
// The constructor keeps the standalone fixture interface; native test keys
// come from CryptoNote's normal generator rather than OpenSSL test seeds.
namespace xgtx_test {
using namespace xgtx_exchange;
inline id hash(const bytes& b){return cryptonote_hash(b);}
inline bool verify(const id& pub,const bytes& message,const signature& sig){return cryptonote_verify(pub,message,sig);}
struct wallet {
  crypto::public_key public_key{};
  crypto::secret_key secret_key{};
  id pub{};
  explicit wallet(std::uint8_t){
    crypto::generate_keys(public_key,secret_key);
    std::memcpy(pub.data(),&public_key,32);
  }
  envelope sign(operation op)const{return cryptonote_sign(op,public_key,secret_key);}
};
}
