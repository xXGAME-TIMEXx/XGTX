#pragma once
#include "../../src/xgtx_exchange/engine.hpp"
#include <openssl/evp.h>
#include <memory>
#include <stdexcept>

namespace xgtx_test {
using namespace xgtx_exchange;
using key_ptr=std::unique_ptr<EVP_PKEY,decltype(&EVP_PKEY_free)>;
using ctx_ptr=std::unique_ptr<EVP_MD_CTX,decltype(&EVP_MD_CTX_free)>;
inline id hash(const bytes& b){id h{};unsigned n=0;if(EVP_Digest(b.data(),b.size(),h.data(),&n,EVP_sha256(),nullptr)!=1 || n!=32)throw std::runtime_error("hash");return h;}
struct wallet {
  key_ptr key{nullptr,EVP_PKEY_free};id pub{};
  explicit wallet(std::uint8_t seed){id secret{};secret.fill(seed);key.reset(EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519,nullptr,secret.data(),32));std::size_t n=32;if(!key || EVP_PKEY_get_raw_public_key(key.get(),pub.data(),&n)!=1 || n!=32)throw std::runtime_error("key");}
  envelope sign(operation op)const{op.sender=pub;envelope e{op,{}};auto m=signing_message(op);ctx_ptr ctx(EVP_MD_CTX_new(),EVP_MD_CTX_free);std::size_t n=64;if(!ctx || EVP_DigestSignInit(ctx.get(),nullptr,nullptr,nullptr,key.get())!=1 || EVP_DigestSign(ctx.get(),e.sig.data(),&n,m.data(),m.size())!=1 || n!=64)throw std::runtime_error("sign");return e;}
};
inline bool verify(const id& pub,const bytes& message,const signature& sig){key_ptr key(EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519,nullptr,pub.data(),32),EVP_PKEY_free);ctx_ptr ctx(EVP_MD_CTX_new(),EVP_MD_CTX_free);return key && ctx && EVP_DigestVerifyInit(ctx.get(),nullptr,nullptr,nullptr,key.get())==1 && EVP_DigestVerify(ctx.get(),sig.data(),64,message.data(),message.size())==1;}
}
