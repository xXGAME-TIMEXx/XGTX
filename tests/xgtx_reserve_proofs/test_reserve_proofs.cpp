#include "../../src/xgtx_exchange/reserve_proofs.hpp"
#include "../../src/xgtx_exchange/native_chain.hpp"
#include "cryptonote_core/tx_verification_utils.h"
#include "../xgtx_exchange/test_native_crypto.hpp"
#include <iostream>
#include <tuple>
using namespace xgtx_exchange;
namespace rp=xgtx_exchange::reserve_proofs;
namespace {
unsigned checks=0;
void check(bool value,const char* label){++checks;if(!value)throw std::runtime_error(label);}
template<class F>void rejects(F f,const char* label){bool failed=false;try{f();}catch(const std::exception&){failed=true;}check(failed,label);}
void run(unsigned bp){
  xgtx_test::wallet owner(1),recipient(2);id chain{};chain.fill(0x72);
  operation op;op.chain=chain;op.deadline=100;op.quantity=600000;op.ticker="PROOF";
  // Existing create intent is ONLY a signature fixture. This test does not add
  // a deposit operation or reinterpret create as a native deposit.
  auto intent=owner.sign(op);rp::domain domain{chain,owner.pub,1};auto reserve=rp::derive(domain,0);
  auto repeated=rp::derive(domain,0);check(repeated.spend==reserve.spend && repeated.amount_key==reserve.amount_key,"deterministic public opening");
  check(reserve.spend!=reserve.amount_key,"separate spend/amount domains");
  auto other=domain;other.nonce++;check(rp::derive(other,0).spend!=reserve.spend,"nonce key separation");
  other=domain;other.chain[0]^=1;check(rp::derive(other,0).spend!=reserve.spend,"chain key separation");
  other=domain;other.owner[0]^=1;check(rp::derive(other,0).spend!=reserve.spend,"owner key separation");
  check(rp::derive(domain,1).spend!=reserve.spend,"output key separation");
  rejects([&]{rp::derive(domain,16);},"invalid reserve index");
  other=domain;other.nonce=0;rejects([&]{rp::derive(other,0);},"zero nonce");
  rct::ctkey secret{},real{};std::tie(secret,real)=rct::ctskpkGen(1000000);
  rct::ctkeyV inSk{secret},outSk;std::vector<rct::xmr_amount> inputs{1000000},outputs{600000,399000};
  rct::ctkeyM ring(1);for(unsigned j=0;j<16;++j){
    if(j==7)ring[0].push_back(real);
    else {rct::ctkey s{},p{};std::tie(s,p)=rct::ctskpkGen(1000000+j);ring[0].push_back(p);}
  }
  rct::keyV destinations{rct::pk2rct(reserve.public_key),rct::pk2rct(recipient.public_key)};
  rct::keyV amount_keys{reserve.amount_key,rct::skGen()};std::vector<unsigned> indices{7};
  rct::RCTConfig config{rct::RangeProofPaddedBulletproof,bp};auto& device=hw::get_device("default");
  cryptonote::transaction original;original.version=2;cryptonote::txin_to_key input;input.key_offsets.assign(16,1);
  crypto::generate_key_image(rct::rct2pk(real.dest),rct::rct2sk(secret.dest),input.k_image);original.vin.push_back(input);
  for(const auto& destination:destinations){cryptonote::tx_out out;out.amount=0;out.target=cryptonote::txout_to_key(rct::rct2pk(destination));original.vout.push_back(out);}
  cryptonote::add_tx_pub_key_to_extra(original,owner.public_key);
  auto prefix=cryptonote::get_transaction_prefix_hash(original);
  auto tx=sign_carrier_transaction(original,intent,owner.public_key,owner.secret_key,
    [&](const std::function<rct::key(const rct::rctSig&)>& finalize){
      return rct::genRctSimpleWithMessageFinalizer(rct::hash2rct(prefix),inSk,destinations,inputs,outputs,1000,ring,amount_keys,indices,outSk,config,device,finalize);
    });
  check(rct::verRctSemanticsSimple(tx.rct_signatures),"native range and conservation proofs");
  check(cryptonote::ver_input_proofs_rings(tx,ring),"native input ring proofs");
  auto verified=rp::inspect_output(tx,domain,0,600000);
  check(verified.quantity==600000 && verified.mask==outSk[0].mask,"native reserve commitment opens to exact quantity and mask");
  check(verified.image!=crypto::key_image{},"deterministic reserve key image");
  rejects([&]{rp::inspect_output(tx,domain,0,600001);},"fabricated reserve amount");
  rejects([&]{rp::inspect_output(tx,domain,0,599999);},"fee confusion changes credited amount");
  rejects([&]{rp::inspect_output(tx,domain,1,399000);},"ordinary change is not reserve");
  rejects([&]{rp::inspect_output(tx,domain,2,1);},"missing output");
  other=domain;other.nonce++;rejects([&]{rp::inspect_output(tx,other,0,600000);},"nonce substitution");
  other=domain;other.owner=recipient.pub;rejects([&]{rp::inspect_output(tx,other,0,600000);},"owner substitution");
  other=domain;other.chain[0]^=1;rejects([&]{rp::inspect_output(tx,other,0,600000);},"cross-chain proof");
  auto changed=tx;changed.rct_signatures.ecdhInfo[0].amount.bytes[0]^=1;changed.invalidate_hashes();
  rejects([&]{rp::inspect_output(changed,domain,0,600000);},"ECDH tamper");
  changed=tx;changed.rct_signatures.outPk[0].mask.bytes[0]^=1;changed.invalidate_hashes();
  rejects([&]{rp::inspect_output(changed,domain,0,600000);},"commitment tamper");
  changed=tx;changed.rct_signatures.txnFee++;changed.invalidate_hashes();
  rejects([&]{rp::inspect_output(changed,domain,0,600000);},"native fee tamper");
  changed=tx;changed.rct_signatures.p.CLSAGs[0].s[0].bytes[0]^=1;
  check(rp::inspect_output(changed,domain,0,600000).quantity==600000,"opening does not pretend to verify input proofs");
  check(!cryptonote::ver_input_proofs_rings(changed,ring),"separate native verifier rejects forged funding proof");
  auto blob=cryptonote::tx_to_blob(tx);cryptonote::transaction parsed;
  check(cryptonote::parse_and_validate_tx_from_blob(cryptonote::blobdata_ref{blob.data(),blob.size()},parsed),"native wire parses");
  check(rp::inspect_output(parsed,domain,0,600000).quantity==600000,"wire reserve commitment opens");
  check(cryptonote::ver_input_proofs_rings(parsed,ring),"wire native input verification");
  check(!rp::has_reserved_input(tx),"ordinary funding does not use reserve bucket");rp::reject_unreleased_inputs(tx);
  // Knowing the public reserve secret must not open the consensus spend gate.
  cryptonote::transaction spend;spend.version=2;cryptonote::txin_to_key reserved_input;reserved_input.amount=rp::bucket;
  reserved_input.k_image=verified.image;reserved_input.key_offsets.assign(16,1);spend.vin.push_back(reserved_input);
  check(rp::has_reserved_input(spend),"reserve bucket spend recognized");
  rejects([&]{rp::reject_unreleased_inputs(spend);},"reserve release fails closed");
  spend.vin.insert(spend.vin.begin(),input);
  rejects([&]{rp::reject_unreleased_inputs(spend);},"reserve input cannot hide behind ordinary input");
  xgtx_exchange::engine exchange(xgtx_exchange::config{chain,recipient.pub,2},cryptonote_hash,cryptonote_verify);
  cryptonote::block block;block.tx_hashes.push_back(cryptonote::get_transaction_hash(spend));
  std::vector<std::pair<cryptonote::transaction,cryptonote::blobdata>> transactions{{spend,{}}};
  rejects([&]{block_intents(block,transactions,exchange);},"actual block projection rejects reserve spend with no carrier");
  transactions[0].first=tx;
  check(block_intents(block,transactions,exchange).size()==1,"ordinary carrier remains valid for projection");
  transactions[0].first.extra.clear();
  check(block_intents(block,transactions,exchange).empty(),"ordinary non-carrier remains valid for projection");
}
}
int main(){try{run(3);run(4);std::cout<<checks<<" checks passed: public reserve commitment openings, signed binding, CLSAG/BulletproofPlus proofs, tamper rejection, wire roundtrip and closed reserve spend gate\n";
  std::cout<<"Synthetic rings only. No output segregation, deposits, native ledger credit, redemption or network activation.\n";
}catch(const std::exception& e){std::cerr<<"FAIL after "<<checks<<" checks: "<<e.what()<<'\n';return 1;}}
