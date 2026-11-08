#include "../../src/xgtx_exchange/native_carrier.hpp"
#include "../xgtx_exchange/test_native_crypto.hpp"
#include <iostream>
using namespace xgtx_exchange;
namespace {
int checks=0;
void check(bool value,const char* name){++checks;if(!value)throw std::runtime_error(name);}
template<class F>void reject(F f,const char* name){bool failed=false;try{f();}catch(const std::exception&){failed=true;}check(failed,name);}
cryptonote::transaction skeleton(const xgtx_test::wallet& a,const xgtx_test::wallet& b){
  // Native-shaped unsigned skeleton, not a consensus-valid mined transaction.
  // These tests check binding, not native RingCT proofs or backing.
  cryptonote::transaction tx;tx.version=2;tx.unlock_time=0;
  cryptonote::txin_to_key input;input.amount=0;input.key_offsets.assign(16,1);std::memcpy(&input.k_image,a.pub.data(),32);tx.vin.push_back(input);
  cryptonote::tx_out output;output.amount=0;output.target=cryptonote::txout_to_key(b.public_key);tx.vout.push_back(output);output.target=cryptonote::txout_to_key(a.public_key);tx.vout.push_back(output);
  tx.rct_signatures.type=rct::RCTTypeCLSAG;tx.rct_signatures.txnFee=1000;tx.rct_signatures.outPk.resize(2);tx.rct_signatures.ecdhInfo.resize(2);
  std::memcpy(&tx.rct_signatures.outPk[0].mask,a.pub.data(),32);std::memcpy(&tx.rct_signatures.outPk[1].mask,b.pub.data(),32);
  cryptonote::add_tx_pub_key_to_extra(tx,a.public_key);return tx;
}
}
int main(){try{
  xgtx_test::wallet alice(1),bob(2),attacker(3);id chain{};chain.fill(0x37);
  operation op;op.chain=chain;op.deadline=100;op.quantity=1000000;op.ticker="GAME";op.decimals=10;
  auto intent=alice.sign(op);auto tx=skeleton(alice,bob);auto missing=tx;
  reject([&]{verify_native_carrier(missing,chain);},"missing carrier");
  seal_native_carrier(tx,intent,alice.public_key,alice.secret_key);
  check(encode_envelope(verify_native_carrier(tx,chain))==encode_envelope(intent),"valid native binding");
  auto fields=carrier_fields(tx.extra);auto i=require_carrier_index(fields);auto payload=boost::get<cryptonote::tx_extra_xgtx_exchange>(fields[i]).data;
  check(encode_carrier(decode_carrier(payload))==payload,"carrier roundtrip");
  for(std::size_t n=0;n<payload.size();++n){auto truncated=payload;truncated.resize(n);reject([&]{decode_carrier(truncated);},"truncated carrier");}
  auto bad=payload;bad.push_back(0);reject([&]{decode_carrier(bad);},"trailing carrier bytes");
  bad=payload;bad[8]=2;reject([&]{decode_carrier(bad);},"unknown carrier version");
  bad=payload;bad[9]=0;bad[10]=0;reject([&]{decode_carrier(bad);},"invalid carrier length");
  id other{};other.fill(0x38);reject([&]{verify_native_carrier(tx,other);},"wrong chain");
  auto mutated=tx;mutated.vout[0].target=cryptonote::txout_to_key(attacker.public_key);reject([&]{verify_native_carrier(mutated,chain);},"redirected native destination");
  mutated=tx;mutated.rct_signatures.txnFee++;reject([&]{verify_native_carrier(mutated,chain);},"changed native mining fee");
  mutated=tx;mutated.rct_signatures.type=rct::RCTTypeBulletproofPlus;reject([&]{verify_native_carrier(mutated,chain);},"changed RingCT type");
  mutated=tx;mutated.rct_signatures.outPk[0].mask.bytes[0]^=1;reject([&]{verify_native_carrier(mutated,chain);},"changed output commitment");
  mutated=tx;mutated.rct_signatures.ecdhInfo[0].amount.bytes[0]^=1;reject([&]{verify_native_carrier(mutated,chain);},"changed encrypted amount");
  mutated=tx;mutated.unlock_time++;reject([&]{verify_native_carrier(mutated,chain);},"changed unlock time");
  mutated=tx;boost::get<cryptonote::txin_to_key>(mutated.vin[0]).key_offsets[0]++;reject([&]{verify_native_carrier(mutated,chain);},"changed native inputs");
  mutated=tx;auto& image=boost::get<cryptonote::txin_to_key>(mutated.vin[0]).k_image;reinterpret_cast<std::uint8_t*>(&image)[0]^=1;reject([&]{verify_native_carrier(mutated,chain);},"changed key image");
  auto changed_fields=fields;auto c=decode_carrier(payload);c.intent.op.quantity++;boost::get<cryptonote::tx_extra_xgtx_exchange>(changed_fields[i]).data=encode_carrier(c);mutated=tx;mutated.extra=serialize_carrier_fields(changed_fields);reject([&]{verify_native_carrier(mutated,chain);},"changed intent");
  changed_fields=fields;c=decode_carrier(payload);c.intent.sig[0]^=1;boost::get<cryptonote::tx_extra_xgtx_exchange>(changed_fields[i]).data=encode_carrier(c);mutated=tx;mutated.extra=serialize_carrier_fields(changed_fields);reject([&]{verify_native_carrier(mutated,chain);},"forged intent signature");
  changed_fields=fields;c=decode_carrier(payload);c.binding[0]^=1;boost::get<cryptonote::tx_extra_xgtx_exchange>(changed_fields[i]).data=encode_carrier(c);mutated=tx;mutated.extra=serialize_carrier_fields(changed_fields);reject([&]{verify_native_carrier(mutated,chain);},"forged binding signature");
  changed_fields=fields;changed_fields.push_back(fields[i]);mutated=tx;mutated.extra=serialize_carrier_fields(changed_fields);reject([&]{verify_native_carrier(mutated,chain);},"duplicate carrier");
  mutated=tx;mutated.extra.push_back(0xff);reject([&]{verify_native_carrier(mutated,chain);},"malformed extra");
  bytes oversized={TX_EXTRA_XGTX_EXCHANGE_TAG,0x81,0x04};oversized.resize(3+513);reject([&]{carrier_fields(oversized);},"bounded native field allocation");
  std::vector<std::uint8_t> sorted;check(cryptonote::sort_tx_extra(tx.extra,sorted,false),"native extra sorting supports carrier");check(sorted==tx.extra,"sorting preserves sealed carrier");
  auto unsigned_tx=skeleton(alice,bob);auto before=cryptonote::get_transaction_prefix_hash(unsigned_tx);
  reject([&]{seal_native_carrier(unsigned_tx,intent,attacker.public_key,attacker.secret_key);},"wrong binding owner");
  check(cryptonote::get_transaction_prefix_hash(unsigned_tx)==before,"failed sealing leaves native prefix unchanged");
  reject([&]{seal_native_carrier(unsigned_tx,intent,alice.public_key,attacker.secret_key);},"wrong secret key");
  mutated=tx;reject([&]{seal_native_carrier(mutated,intent,alice.public_key,alice.secret_key);},"second attach rejected");
  auto invalid=skeleton(alice,bob);invalid.version=1;reject([&]{seal_native_carrier(invalid,intent,alice.public_key,alice.secret_key);},"legacy transaction rejected");
  invalid=skeleton(alice,bob);invalid.rct_signatures.p.CLSAGs.resize(1);reject([&]{seal_native_carrier(invalid,intent,alice.public_key,alice.secret_key);},"already ring-signed transaction rejected");
  invalid=skeleton(alice,bob);invalid.rct_signatures.type=rct::RCTTypeNull;reject([&]{seal_native_carrier(invalid,intent,alice.public_key,alice.secret_key);},"null RingCT rejected");
  invalid=skeleton(alice,bob);invalid.rct_signatures.outPk.pop_back();reject([&]{seal_native_carrier(invalid,intent,alice.public_key,alice.secret_key);},"commitment count mismatch");
  invalid=skeleton(alice,bob);invalid.rct_signatures.ecdhInfo.pop_back();reject([&]{seal_native_carrier(invalid,intent,alice.public_key,alice.secret_key);},"ECDH count mismatch");
  invalid=skeleton(alice,bob);invalid.rct_signatures.type=rct::RCTTypeBulletproofPlus;seal_native_carrier(invalid,intent,alice.public_key,alice.secret_key);check(encode_envelope(verify_native_carrier(invalid,chain))==encode_envelope(intent),"BulletproofPlus base supported");
  invalid=skeleton(alice,bob);cryptonote::tx_extra_padding padding{};padding.size=3;auto padded_fields=carrier_fields(invalid.extra);padded_fields.push_back(padding);invalid.extra=serialize_carrier_fields(padded_fields);seal_native_carrier(invalid,intent,alice.public_key,alice.secret_key);check(encode_envelope(verify_native_carrier(invalid,chain))==encode_envelope(intent),"terminal padding supported");
  engine e(xgtx_exchange::config{chain,bob.pub,1,1024},cryptonote_hash,cryptonote_verify);state s;
  auto result=e.apply(s,verify_native_carrier(tx,chain),1);check(s.assets.count(result.asset)==1,"verified carrier feeds engine");
  auto snapshot=serialize(s);reject([&]{e.apply(s,verify_native_carrier(tx,chain),1);},"carrier intent replay rejected by state");check(serialize(s)==snapshot,"replay does not change state");
  std::cout<<checks<<" checks passed: native carrier codec, two signatures, transaction binding, tamper rejection, replay\n";
  std::cout<<"Unsigned native skeleton tests only; no RingCT proof validation, native bridge or node consensus activation.\n";
  return 0;
}catch(const std::exception& ex){std::cerr<<"FAIL after "<<checks<<" checks: "<<ex.what()<<"\n";return 1;}}
