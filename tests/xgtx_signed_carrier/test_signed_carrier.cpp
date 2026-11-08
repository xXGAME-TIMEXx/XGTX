#include "../../src/xgtx_exchange/native_signing.hpp"
#include "cryptonote_core/tx_verification_utils.h"
#include "../xgtx_exchange/test_native_crypto.hpp"
#include <iostream>
#include <tuple>
using namespace xgtx_exchange;
namespace {
int checks=0;
void check(bool ok,const char* name){++checks;if(!ok)throw std::runtime_error(name);}
template<class F>void reject(F f,const char* name){bool failed=false;try{f();}catch(const std::exception&){failed=true;}check(failed,name);}
void run(unsigned bp){
  xgtx_test::wallet owner(1),recipient(2);id chain{};chain.fill(0x49);
  operation op;op.chain=chain;op.deadline=100;op.quantity=1000000;op.ticker="GAME";auto intent=owner.sign(op);
  rct::ctkey secret{},real{};std::tie(secret,real)=rct::ctskpkGen(1000000);
  rct::ctkeyV inSk{secret},outSk;std::vector<rct::xmr_amount> inamounts{1000000},outamounts{600000,399000};
  rct::ctkeyM ring(1);for(unsigned i=0;i<16;++i){
    if(i==7)ring[0].push_back(real);
    else {rct::ctkey sk{},pk{};std::tie(sk,pk)=rct::ctskpkGen(1000000+i);ring[0].push_back(pk);}
  }
  rct::keyV destinations{rct::pk2rct(recipient.public_key),rct::pk2rct(owner.public_key)};
  rct::keyV amount_keys{rct::skGen(),rct::skGen()};std::vector<unsigned> index{7};
  rct::RCTConfig config{rct::RangeProofPaddedBulletproof,bp};auto& device=hw::get_device("default");
  cryptonote::transaction original;original.version=2;
  cryptonote::txin_to_key input;input.key_offsets.assign(16,1);
  crypto::generate_key_image(rct::rct2pk(real.dest),rct::rct2sk(secret.dest),input.k_image);original.vin.push_back(input);
  for(const auto& dest:destinations){cryptonote::tx_out out;out.amount=0;out.target=cryptonote::txout_to_key(rct::rct2pk(dest));original.vout.push_back(out);}
  cryptonote::add_tx_pub_key_to_extra(original,owner.public_key);
  auto before=cryptonote::get_transaction_prefix_hash(original);
  auto generate=[&](const std::function<rct::key(const rct::rctSig&)>& finalize){
    return rct::genRctSimpleWithMessageFinalizer(rct::hash2rct(before),inSk,destinations,inamounts,outamounts,1000,ring,amount_keys,index,outSk,config,device,finalize);
  };
  auto tx=sign_carrier_transaction(original,intent,owner.public_key,owner.secret_key,generate);
  check(cryptonote::get_transaction_prefix_hash(original)==before,"signing leaves original untouched");
  check(tx.rct_signatures.type==(bp==3?rct::RCTTypeCLSAG:rct::RCTTypeBulletproofPlus),"requested native proof type");
  check(encode_envelope(verify_native_carrier(tx,chain))==encode_envelope(intent),"both carrier signatures valid after native signing");
  check(rct::verRctSemanticsSimple(tx.rct_signatures),"range proofs and commitment conservation");
  check(cryptonote::ver_input_proofs_rings(tx,ring),"native input ring verification accepts sealed prefix");
  rct::key mask{};check(rct::decodeRct(tx.rct_signatures,amount_keys[0],0,mask,device)==600000,"recipient encrypted amount decodes");
  check(rct::decodeRct(tx.rct_signatures,amount_keys[1],1,mask,device)==399000,"change encrypted amount decodes");
  auto blob=cryptonote::tx_to_blob(tx);cryptonote::transaction parsed;
  check(cryptonote::parse_and_validate_tx_from_blob(cryptonote::blobdata_ref{blob.data(),blob.size()},parsed),"fully signed native transaction parses");
  check(encode_envelope(verify_native_carrier(parsed,chain))==encode_envelope(intent),"wire carrier signatures valid");
  check(cryptonote::ver_input_proofs_rings(parsed,ring),"native input verification after wire expansion");
  check(rct::verRctSemanticsSimple(parsed.rct_signatures),"wire range proofs and conservation");
  auto changed=tx;changed.unlock_time++;changed.invalidate_hashes();reject([&]{verify_native_carrier(changed,chain);},"prefix tamper breaks carrier");
  check(!cryptonote::ver_input_proofs_rings(changed,ring),"prefix tamper breaks native ring signature");
  changed=tx;changed.rct_signatures.txnFee++;changed.invalidate_hashes();reject([&]{verify_native_carrier(changed,chain);},"fee tamper breaks carrier");
  check(!rct::verRctSemanticsSimple(changed.rct_signatures),"fee tamper breaks native conservation");
  changed=tx;changed.rct_signatures.p.CLSAGs[0].s[0].bytes[0]^=1;changed.invalidate_hashes();
  check(encode_envelope(verify_native_carrier(changed,chain))==encode_envelope(intent),"carrier intentionally excludes prunable native signature");
  check(!cryptonote::ver_input_proofs_rings(changed,ring),"independent native proof check rejects forged CLSAG");
  reject([&]{sign_carrier_transaction(original,intent,owner.public_key,recipient.secret_key,generate);},"wrong owner secret cannot finalize");
  check(cryptonote::get_transaction_prefix_hash(original)==before,"failed finalization atomic");
  reject([&]{sign_carrier_transaction(original,intent,owner.public_key,owner.secret_key,
    [](const std::function<rct::key(const rct::rctSig&)>&){return rct::rctSig{};});},"generator must invoke final-message callback");
  reject([&]{sign_carrier_transaction(original,intent,owner.public_key,owner.secret_key,
    [&](const std::function<rct::key(const rct::rctSig&)>& finalize){auto result=generate(finalize);result.txnFee++;return result;});},"base cannot change after finalization");
  reject([&]{sign_carrier_transaction(original,intent,owner.public_key,owner.secret_key,
    [&](const std::function<rct::key(const rct::rctSig&)>& finalize){auto result=generate(finalize);result.message.bytes[0]^=1;return result;});},"generator must preserve finalized prefix message");
  // Existing entry point still produces and verifies an ordinary native RCT sig.
  auto ordinary=rct::genRctSimple(rct::hash2rct(before),inSk,destinations,inamounts,outamounts,1000,ring,amount_keys,index,outSk,config,device);
  check(rct::verRctSimple(ordinary),"existing signing interface remains valid");
}
}
int main(){try{run(3);run(4);std::cout<<checks<<" checks passed: CLSAG/BulletproofPlus carrier signing, native range/conservation/ring proofs, encrypted amounts, wire roundtrip, tamper rejection\n";
  std::cout<<"Synthetic input rings with known keys/amounts; no funded chain outputs, wallet relay, native XGTX backing or mainnet activation.\n";return 0;
}catch(const std::exception& ex){std::cerr<<"FAIL after "<<checks<<" checks: "<<ex.what()<<"\n";return 1;}}
