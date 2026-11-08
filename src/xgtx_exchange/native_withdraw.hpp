#pragma once
#include "native_deposit.hpp"
#include <set>
namespace xgtx_exchange {
// Bind all ordinary payout output keys in order. Wallets must derive normal
// one-time recipient keys before signing the intent; full carrier binding also
// freezes their commitments/ECDH data and the fee. No amount-key disclosure is
// required for the wallet's ordinary payout outputs.
inline id withdrawal_payout_binding(const cryptonote::transaction_prefix& tx,const id& chain,bool has_change){
  carrier_require(chain!=id{} && tx.vout.size()==2,"withdrawal requires two native outputs");
  bytes data={'X','G','T','X','-','P','A','Y','O','U','T','-','K','E','Y','S',0,1};
  data.insert(data.end(),chain.begin(),chain.end());data.push_back(has_change?1:2);
  std::set<id> keys;
  for(std::size_t i=has_change?1:0;i<tx.vout.size();++i){
    crypto::public_key pub{};carrier_require(cryptonote::get_output_public_key(tx.vout[i],pub) && crypto::check_key(pub),"invalid native payout key");
    id key{};std::memcpy(key.data(),&pub,32);carrier_require(keys.insert(key).second,"duplicate native payout key");
    data.push_back(static_cast<std::uint8_t>(i));data.insert(data.end(),key.begin(),key.end());
  }
  auto binding=cryptonote_hash(data);carrier_require(binding!=id{},"zero native payout binding");return binding;
}
struct native_reserve_source {
  cryptonote::transaction origin;
  id transaction{},prunable_hash{};
  std::uint32_t index=0;
  rct::ctkey stored{};
};
inline reserve_proofs::verified_output verify_reserve_source(const native_reserve_source& source,const id& chain){
  crypto::hash hash{};
  if(source.origin.pruned){crypto::hash prunable{};std::memcpy(&prunable,source.prunable_hash.data(),32);carrier_require(cryptonote::get_pruned_transaction_hash(source.origin,prunable,hash),"cannot authenticate pruned reserve origin");}
  else hash=cryptonote::get_transaction_hash(source.origin);
  id actual{};std::memcpy(actual.data(),&hash,32);
  carrier_require(actual==source.transaction,"native reserve origin hash mismatch");
  const auto intent=verify_native_carrier(source.origin,chain);const auto& op=intent.op;amount quantity=0;
  if(op.type==kind::native_deposit){carrier_require(source.index<op.limit,"reserve source is not a deposit output");quantity=deposit_part(op.quantity,op.limit,source.index);}
  else {carrier_require(op.type==kind::native_withdraw && op.limit && source.index==0,"reserve source is not withdrawal change");quantity=op.limit;}
  carrier_require(source.index<source.origin.vout.size() && source.origin.vout.size()<=16,"invalid reserve source output index");
  if(op.type==kind::native_deposit)carrier_require((op.limit==1 || op.limit==16) && source.origin.vout.size()>=op.limit,"invalid deposit source shape");
  else carrier_require(source.origin.vout.size()==2,"invalid withdrawal source shape");
  reserve_proofs::domain domain{chain,op.sender,op.nonce};auto result=reserve_proofs::inspect_output(source.origin,domain,source.index,quantity);
  crypto::public_key pub{};carrier_require(cryptonote::get_output_public_key(source.origin.vout[source.index],pub),"missing reserve source public key");
  carrier_require(source.stored.dest==rct::pk2rct(pub) && source.stored.mask==source.origin.rct_signatures.outPk[source.index].mask,"native reserve database record mismatch");return result;
}
// Lookup resolves CONFIRMED reserved bucket records. Spent checks the native
// database key-image set. Normal native ring/range/unlock consensus validation
// remains mandatory; this adapter adds reserve origin + signed ledger rules.
template<class Lookup,class Spent>
native_effect withdrawal_native_effect(const cryptonote::transaction& tx,const engine& e,Lookup lookup,Spent spent){
  carrier_require(e.settings().native_backing,"native backing disabled");
  auto intent=verify_native_carrier(tx,e.settings().chain);const auto& op=intent.op;
  carrier_require(op.type==kind::native_withdraw && tx.vin.size()<=16 && !tx.vin.empty(),"native withdrawal shape");
  carrier_require(tx.vout.size()==2 && op.quantity>tx.rct_signatures.txnFee,"withdrawal has no positive payout after mining fee");
  const auto payout=withdrawal_payout_binding(tx,e.settings().chain,op.limit!=0);
  carrier_require(op.recipient==payout,"withdrawal payout keys differ from signed intent");
  deposit_output_mask(tx,e.settings().chain);
  wide consumed=0;std::set<id> images;
  std::map<amount,reserve_proofs::verified_output> validated_sources;
  for(const auto& value:tx.vin){
    carrier_require(value.type()==typeid(cryptonote::txin_to_key),"native reserve input type");
    const auto& input=boost::get<cryptonote::txin_to_key>(value);
    carrier_require(input.amount==reserve_policy::bucket && input.key_offsets.size()==16,"withdrawal must use only sixteen-member reserve rings");
    id image{};std::memcpy(image.data(),&input.k_image,32);
    carrier_require(images.insert(image).second && !spent(input.k_image),"duplicate or spent native reserve key image");
    amount absolute=0,real_quantity=0;unsigned matched=0;
    for(std::size_t j=0;j<input.key_offsets.size();++j){
      const auto delta=input.key_offsets[j];carrier_require((j==0 || delta) && delta<=std::numeric_limits<amount>::max()-absolute,"invalid reserve ring offset");absolute+=delta;
      auto cached=validated_sources.find(absolute);
      if(cached==validated_sources.end())cached=validated_sources.emplace(absolute,verify_reserve_source(lookup(absolute),e.settings().chain)).first;
      if(cached->second.image==input.k_image){++matched;real_quantity=cached->second.quantity;}
    }
    carrier_require(matched==1,"reserve input key image has no unique confirmed origin");
    consumed+=real_quantity;carrier_require(consumed<=std::numeric_limits<amount>::max(),"native reserve input sum overflow");
  }
  carrier_require(consumed>=op.limit && consumed-op.limit==op.quantity,"native withdrawal net reserve decrease differs from signed debit");
  carrier_require(rct::verRctSemanticsSimple(tx.rct_signatures),"native withdrawal range/conservation proofs invalid");
  auto hash=cryptonote::get_transaction_hash(tx);id transaction{};std::memcpy(transaction.data(),&hash,32);
  return {transaction,cryptonote_hash(encode_envelope(intent)),payout,op.limit,static_cast<amount>(consumed)};
}
}
