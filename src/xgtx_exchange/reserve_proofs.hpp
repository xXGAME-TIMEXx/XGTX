#pragma once
#include "native_signing.hpp"
#include "reserve_guard.hpp"
#include "device/device.hpp"
#include <limits>

namespace xgtx_exchange::reserve_proofs {
// This is a prospective segregated native DB amount bucket, NOT a public coin
// amount. No output routing or deposits are enabled by this module.
using reserve_policy::bucket;
using reserve_policy::has_reserved_input;
using reserve_policy::reject_unreleased_inputs;
struct domain {id chain{},owner{};amount nonce=0;};
struct opening {rct::key spend{},amount_key{};crypto::public_key public_key{};};
inline rct::key scalar(const domain& d,std::uint32_t index,std::uint8_t purpose){
  carrier_require(d.chain!=id{} && d.owner!=id{} && d.nonce && index<16,"invalid reserve key domain");
  bytes data={'X','G','T','X','-','R','E','S','E','R','V','E',0,1,purpose};
  data.insert(data.end(),d.chain.begin(),d.chain.end());data.insert(data.end(),d.owner.begin(),d.owner.end());
  for(unsigned j=0;j<8;++j)data.push_back(static_cast<std::uint8_t>(d.nonce>>(8*j)));
  for(unsigned j=0;j<4;++j)data.push_back(static_cast<std::uint8_t>(index>>(8*j)));
  rct::key out{};rct::hash_to_scalar(out,data.data(),data.size());
  carrier_require(out!=rct::zero(),"zero reserve scalar");return out;
}
// Both scalars are PUBLIC by design. Never send real funds to these keys until
// DB segregation AND consensus release authorization have been implemented and
// independently tested. A public key alone cannot protect a reserve.
inline opening derive(const domain& d,std::uint32_t index){
  opening out;out.spend=scalar(d,index,1);out.amount_key=scalar(d,index,2);
  carrier_require(crypto::secret_key_to_public_key(rct::rct2sk(out.spend),out.public_key),"invalid reserve public key");
  return out;
}
struct verified_output {amount quantity=0;rct::key mask{};crypto::key_image image{};};
// Authenticates a public opening against native ECDH/commitment data and the
// owner-bound carrier. Does NOT prove funding, input ownership, output locking,
// unspentness, block inclusion or deposit-operation semantics. A native adapter
// must verify all of those separately before passing any effect to accounting.
inline verified_output inspect_output(const cryptonote::transaction& tx,const domain& d,
    std::uint32_t index,amount expected){
  carrier_require(expected && tx.vout.size()<=16 && index<tx.vout.size(),"reserve output index/quantity");
  const auto intent=verify_native_carrier(tx,d.chain);
  carrier_require(intent.op.sender==d.owner && intent.op.nonce==d.nonce,"reserve owner/nonce mismatch");
  const auto key=derive(d,index);crypto::public_key actual{};
  carrier_require(cryptonote::get_output_public_key(tx.vout[index],actual) && actual==key.public_key,"wrong reserve output key");
  auto& device=hw::get_device("default");verified_output out;
  out.quantity=rct::decodeRct(tx.rct_signatures,key.amount_key,index,out.mask,device);
  carrier_require(out.quantity==expected,"reserve quantity differs from commitment");
  carrier_require(out.mask==rct::genCommitmentMask(key.amount_key),"reserve mask differs from public opening");
  crypto::generate_key_image(key.public_key,rct::rct2sk(key.spend),out.image);
  return out;
}
}
