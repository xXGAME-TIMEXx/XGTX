#pragma once
#include "engine.hpp"
#include "serialization/string.h"
#include "cryptonote_crypto.hpp"
#include "cryptonote_basic/cryptonote_basic.h"
#include "cryptonote_basic/cryptonote_format_utils.h"
#include "cryptonote_basic/tx_extra.h"
#include <algorithm>
#include <cstring>
#include <sstream>

namespace xgtx_exchange {
// A native transaction carries two authorizations: the existing signed exchange
// intent, and a second signature binding that intent to the native skeleton.
// This module does not verify RingCT proofs or activate exchange consensus.
struct native_carrier { envelope intent; signature binding{}; };
inline void carrier_require(bool ok,const char* message){if(!ok)throw std::invalid_argument(message);}
inline bytes carrier_prefix(){return {'X','G','T','X','C','A','R',0,1};}
inline bytes encode_carrier(const native_carrier& c){
  auto body=encode_envelope(c.intent);auto out=carrier_prefix();
  out.push_back(static_cast<std::uint8_t>(body.size()));
  out.push_back(static_cast<std::uint8_t>(body.size()>>8));
  out.insert(out.end(),body.begin(),body.end());
  out.insert(out.end(),c.binding.begin(),c.binding.end());return out;
}
inline native_carrier decode_carrier(const bytes& b){
  carrier_require(b.size()>=351 && b.size()<=363,"carrier size");auto prefix=carrier_prefix();
  carrier_require(std::equal(prefix.begin(),prefix.end(),b.begin()),"carrier domain/version");
  std::size_t n=std::size_t(b[9])|(std::size_t(b[10])<<8);
  carrier_require(n>=276 && n<=288 && b.size()==11+n+64,"carrier envelope length");
  native_carrier c;c.intent=decode_envelope(bytes(b.begin()+11,b.begin()+11+n));
  std::copy_n(b.begin()+11+n,64,c.binding.begin());return c;
}
using extra_fields=std::vector<cryptonote::tx_extra_field>;
inline extra_fields carrier_fields(const std::vector<std::uint8_t>& extra){
  carrier_require(extra.size()<=16384,"carrier extra size limit");extra_fields fields;
  carrier_require(cryptonote::parse_tx_extra(extra,fields),"malformed native extra");return fields;
}
inline bytes serialize_carrier_fields(const extra_fields& fields){
  std::ostringstream out;binary_archive<true> ar(out);
  for(auto field:fields)carrier_require(::do_serialize(ar,field),"cannot serialize extra field");
  carrier_require(ar.good(),"cannot serialize extra");auto s=out.str();
  carrier_require(s.size()<=16384,"carrier extra size limit");return bytes(s.begin(),s.end());
}
inline std::size_t require_carrier_index(const extra_fields& fields){
  std::size_t count=0,index=0;
  for(std::size_t i=0;i<fields.size();++i)if(fields[i].type()==typeid(cryptonote::tx_extra_xgtx_exchange)){++count;index=i;}
  carrier_require(count==1,"expected exactly one exchange carrier");return index;
}
inline void carrier_native_shape(const cryptonote::transaction& tx){
  carrier_require(tx.version==2 && !tx.vin.empty() && tx.vout.size()>=2,"carrier requires ordinary v2 transaction");
  for(auto& input:tx.vin)carrier_require(input.type()==typeid(cryptonote::txin_to_key),"carrier input type");
  for(auto& output:tx.vout){
    crypto::public_key key{};
    carrier_require(output.amount==0 && cryptonote::get_output_public_key(output,key) && crypto::check_key(key),"carrier output type/key");
  }
  carrier_require(tx.rct_signatures.type==rct::RCTTypeCLSAG || tx.rct_signatures.type==rct::RCTTypeBulletproofPlus,"carrier RingCT type");
}
inline bytes carrier_binding_message(const cryptonote::transaction& tx){
  carrier_native_shape(tx);auto fields=carrier_fields(tx.extra);auto index=require_carrier_index(fields);
  carrier_require(serialize_carrier_fields(fields)==tx.extra,"noncanonical carrier extra");
  auto& field=boost::get<cryptonote::tx_extra_xgtx_exchange>(fields[index]);
  auto carrier=decode_carrier(field.data);carrier.binding.fill(0);field.data=encode_carrier(carrier);
  cryptonote::transaction_prefix prefix=tx;prefix.extra=serialize_carrier_fields(fields);
  auto prefix_hash=cryptonote::get_transaction_prefix_hash(prefix);
  // Freeze the commitments, serialized ECDH data, type and actual mining fee.
  // Exclude prunable native signatures to avoid a circular signing dependency.
  rct::rctSigBase base=tx.rct_signatures;std::ostringstream out;binary_archive<true> ar(out);
  carrier_require(base.serialize_rctsig_base(ar,tx.vin.size(),tx.vout.size()) && ar.good(),"malformed carrier RingCT base");
  auto serialized=out.str();auto base_hash=crypto::cn_fast_hash(serialized.data(),serialized.size());
  bytes message={'X','G','T','X','-','C','A','R','R','I','E','R','-','B','I','N','D',0,1};
  auto p=reinterpret_cast<const std::uint8_t*>(&prefix_hash);message.insert(message.end(),p,p+32);
  p=reinterpret_cast<const std::uint8_t*>(&base_hash);message.insert(message.end(),p,p+32);return message;
}
inline envelope verify_native_carrier(const cryptonote::transaction& tx,const id& expected_chain){
  auto fields=carrier_fields(tx.extra);auto index=require_carrier_index(fields);
  auto c=decode_carrier(boost::get<cryptonote::tx_extra_xgtx_exchange>(fields[index]).data);
  carrier_require(expected_chain!=id{} && c.intent.op.chain==expected_chain,"carrier chain mismatch");
  carrier_require(cryptonote_verify(c.intent.op.sender,signing_message(c.intent.op),c.intent.sig),"carrier intent signature");
  carrier_require(cryptonote_verify(c.intent.op.sender,carrier_binding_message(tx),c.binding),"native carrier binding signature");
  return c.intent;
}
inline void seal_native_carrier(cryptonote::transaction& original,const envelope& intent,const crypto::public_key& pub,const crypto::secret_key& secret){
  carrier_native_shape(original);crypto::public_key derived{};id sender{};std::memcpy(sender.data(),&pub,32);
  carrier_require(original.signatures.empty() && original.rct_signatures.p.CLSAGs.empty() && original.rct_signatures.p.MGs.empty(),"seal carrier before native ring signing");
  carrier_require(crypto::secret_key_to_public_key(secret,derived) && std::memcmp(&derived,&pub,32)==0,"carrier signing key mismatch");
  carrier_require(intent.op.sender==sender && cryptonote_verify(sender,signing_message(intent.op),intent.sig),"carrier intent owner/signature");
  auto fields=carrier_fields(original.extra);
  for(auto& f:fields)carrier_require(f.type()!=typeid(cryptonote::tx_extra_xgtx_exchange),"carrier already attached");
  cryptonote::transaction candidate=original;
  std::vector<std::uint8_t> sorted;
  carrier_require(cryptonote::sort_tx_extra(candidate.extra,sorted,false),"cannot canonicalize native extra");
  fields=carrier_fields(sorted);
  native_carrier c{intent,{}};cryptonote::tx_extra_xgtx_exchange field;field.data=encode_carrier(c);
  auto padding=std::find_if(fields.begin(),fields.end(),[](const auto& f){return f.type()==typeid(cryptonote::tx_extra_padding);});
  fields.insert(padding,field);candidate.extra=serialize_carrier_fields(fields);
  auto message=carrier_binding_message(candidate);auto hash=crypto::cn_fast_hash(message.data(),message.size());crypto::signature signed_binding{};
  crypto::generate_signature(hash,pub,secret,signed_binding);std::memcpy(c.binding.data(),&signed_binding,64);
  auto index=require_carrier_index(fields);boost::get<cryptonote::tx_extra_xgtx_exchange>(fields[index]).data=encode_carrier(c);
  candidate.extra=serialize_carrier_fields(fields);candidate.invalidate_hashes();
  verify_native_carrier(candidate,intent.op.chain);original=std::move(candidate);
}
}
