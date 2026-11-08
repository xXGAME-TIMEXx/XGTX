#include "../../src/xgtx_exchange/native_chain.hpp"
#include "blockchain_db/lmdb/db_lmdb.h"
#include "cryptonote_basic/hardfork.h"
#include "cryptonote_core/tx_verification_utils.h"
#include "../xgtx_exchange/test_native_crypto.hpp"
#include <boost/filesystem.hpp>
#include <iostream>
#include <memory>
using namespace xgtx_exchange;
namespace {
unsigned checks=0;
void check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
template<class F>void rejects(F fn,const char* message){bool failed=false;try{fn();}catch(const std::exception&){failed=true;}check(failed,message);}
struct directory {
  boost::filesystem::path path=boost::filesystem::temp_directory_path()/boost::filesystem::unique_path("xgtx-native-deposit-%%%%-%%%%-%%%%");
  directory(){boost::filesystem::create_directories(path);}
  ~directory(){boost::system::error_code error;boost::filesystem::remove_all(path,error);}
};
cryptonote::block block(const crypto::hash& parent,unsigned height,const crypto::public_key& miner){
  cryptonote::block out;out.major_version=16;out.minor_version=16;out.timestamp=1000+height;out.prev_id=parent;out.nonce=height+1;
  out.miner_tx.version=2;out.miner_tx.unlock_time=height+60;cryptonote::txin_gen in;in.height=height;out.miner_tx.vin.push_back(in);
  cryptonote::tx_out output;output.amount=1000000;output.target=cryptonote::txout_to_key(miner);out.miner_tx.vout.push_back(output);
  cryptonote::add_tx_pub_key_to_extra(out.miner_tx,miner);return out;
}
void append(cryptonote::BlockchainLMDB& db,const cryptonote::block& candidate,
    const std::vector<std::pair<cryptonote::transaction,cryptonote::blobdata>>& transactions={},bool commit=true){
  db.block_wtxn_start();try{const auto blob=cryptonote::block_to_blob(candidate);
    db.add_block({candidate,blob},blob.size(),blob.size(),cryptonote::difficulty_type(1),1000000*(candidate.miner_tx.vin.empty()?1:db.height()+1),transactions);
    if(commit)db.block_wtxn_stop();else db.block_wtxn_abort();
  }catch(...){db.block_wtxn_abort();throw;}
}
rct::ctkeyM ring(cryptonote::BlockchainLMDB& db,amount bucket){
  rct::ctkeyM result(1);for(unsigned i=0;i<16;++i){auto key=db.get_output_key(bucket,i,true);result[0].push_back({rct::pk2rct(key.pubkey),key.commitment});}return result;
}
cryptonote::transaction deposit(cryptonote::BlockchainLMDB& db,const xgtx_exchange::config& cfg,const xgtx_test::wallet& owner,const xgtx_test::wallet& funding,amount claimed=999000){
  operation op;op.type=kind::native_deposit;op.chain=cfg.chain;op.deadline=1000;op.quantity=claimed;op.limit=16;auto intent=owner.sign(op);
  reserve_proofs::domain domain{cfg.chain,owner.pub,1};rct::keyV destinations,amount_keys;std::vector<rct::xmr_amount> outamounts;
  cryptonote::transaction original;original.version=2;cryptonote::txin_to_key in;in.key_offsets.assign(16,1);in.key_offsets[0]=0;
  crypto::generate_key_image(funding.public_key,funding.secret_key,in.k_image);original.vin.push_back(in);
  for(unsigned i=0;i<16;++i){auto key=reserve_proofs::derive(domain,i);destinations.push_back(rct::pk2rct(key.public_key));amount_keys.push_back(key.amount_key);
    outamounts.push_back(deposit_part(999000,16,i));cryptonote::tx_out out;out.amount=0;out.target=cryptonote::txout_to_key(key.public_key);original.vout.push_back(out);}
  cryptonote::add_tx_pub_key_to_extra(original,owner.public_key);auto prefix=cryptonote::get_transaction_prefix_hash(original);
  rct::ctkeyV secrets{{rct::sk2rct(funding.secret_key),rct::identity()}},out_secrets;std::vector<rct::xmr_amount> inamounts{1000000};std::vector<unsigned> indices{0};auto mix=ring(db,0);
  auto& device=hw::get_device("default");rct::RCTConfig proof{rct::RangeProofPaddedBulletproof,4};
  auto tx=sign_carrier_transaction(original,intent,owner.public_key,owner.secret_key,
    [&](const std::function<rct::key(const rct::rctSig&)>& finalize){return rct::genRctSimpleWithMessageFinalizer(rct::hash2rct(prefix),secrets,destinations,inamounts,outamounts,1000,mix,amount_keys,indices,out_secrets,proof,device,finalize);});
  check(cryptonote::ver_input_proofs_rings(tx,mix),"deposit spends actual database funding output with valid native proof");return tx;
}
cryptonote::transaction public_key_attack(cryptonote::BlockchainLMDB& db,const cryptonote::transaction& deposit_tx,const xgtx_exchange::config& cfg,const xgtx_test::wallet& owner,amount claimed_bucket){
  reserve_proofs::domain domain{cfg.chain,owner.pub,1};auto opening=reserve_proofs::derive(domain,0);auto value=reserve_proofs::inspect_output(deposit_tx,domain,0,deposit_part(999000,16,0));
  cryptonote::transaction tx;tx.version=2;cryptonote::txin_to_key in;in.amount=claimed_bucket;in.key_offsets.assign(16,1);in.key_offsets[0]=0;in.k_image=value.image;tx.vin.push_back(in);
  rct::keyV destinations{rct::pk2rct(owner.public_key),rct::pk2rct(owner.public_key)},amount_keys{rct::skGen(),rct::skGen()};
  for(const auto& key:destinations){cryptonote::tx_out out;out.amount=0;out.target=cryptonote::txout_to_key(rct::rct2pk(key));tx.vout.push_back(out);}
  cryptonote::add_tx_pub_key_to_extra(tx,owner.public_key);auto prefix=cryptonote::get_transaction_prefix_hash(tx);
  rct::ctkeyV secrets{{opening.spend,value.mask}},out_secrets;std::vector<rct::xmr_amount> inamounts{value.quantity},outamounts{10000,value.quantity-11000};std::vector<unsigned> indices{0};
  auto& device=hw::get_device("default");rct::RCTConfig proof{rct::RangeProofPaddedBulletproof,4};
  tx.rct_signatures=rct::genRctSimple(rct::hash2rct(prefix),secrets,destinations,inamounts,outamounts,1000,ring(db,reserve_policy::bucket),amount_keys,indices,out_secrets,proof,device);tx.invalidate_hashes();return tx;
}
}
int main(){try{
  directory temp;xgtx_test::wallet owner(1),treasury(2);std::vector<std::unique_ptr<xgtx_test::wallet>> funding;
  for(unsigned i=0;i<16;++i)funding.emplace_back(new xgtx_test::wallet(i+3));
  auto genesis=block(crypto::null_hash,0,funding[0]->public_key);xgtx_exchange::config cfg{native_id(cryptonote::get_block_hash(genesis)),treasury.pub,1,1024,true};
  cryptonote::transaction tx;cryptonote::block candidate;bytes confirmed;crypto::key_image spent_image{};
  {
    cryptonote::BlockchainLMDB db;db.configure_exchange_for_tests(cfg);db.open(temp.path.string());cryptonote::HardFork hf(db,16,0);hf.init();db.set_hard_fork(&hf);
    append(db,genesis);for(unsigned h=1;h<80;++h)append(db,block(db.top_block_hash(),h,h<16?funding[h]->public_key:owner.public_key));
    check(db.height()==80 && db.get_num_outputs(0)==80,"eighty native fixture coinbases stored");
    check(db.get_num_outputs(reserve_policy::bucket)==0 && db.exchange_state_for_tests().native_locked==0,"no reserve output or native credit before deposit");
    auto false_claim=deposit(db,cfg,owner,*funding[0],999001);auto bad_block=block(db.top_block_hash(),80,owner.public_key);bad_block.tx_hashes.push_back(cryptonote::get_transaction_hash(false_claim));
    auto before=serialize(db.exchange_state_for_tests());rejects([&]{append(db,bad_block,{{false_claim,cryptonote::tx_to_blob(false_claim)}});},"signed deposit claim exceeding native commitment rejects");
    check(db.height()==80 && serialize(db.exchange_state_for_tests())==before && db.get_num_outputs(reserve_policy::bucket)==0,"false claim rolls back native and exchange state");
    tx=deposit(db,cfg,owner,*funding[0]);spent_image=boost::get<cryptonote::txin_to_key>(tx.vin[0]).k_image;
    candidate=block(db.top_block_hash(),80,owner.public_key);candidate.tx_hashes.push_back(cryptonote::get_transaction_hash(tx));
    std::vector<std::pair<cryptonote::transaction,cryptonote::blobdata>> transactions{{tx,cryptonote::tx_to_blob(tx)}};
    append(db,candidate,transactions,false);check(db.height()==80 && db.get_num_outputs(0)==80 && db.get_num_outputs(reserve_policy::bucket)==0 && !db.has_key_image(spent_image),"aborted deposit leaves no native input, reserve output or ledger credit");
    append(db,candidate,transactions);check(db.height()==81 && db.get_num_outputs(0)==81,"reserve outputs excluded from ordinary output bucket");
    check(db.get_num_outputs(reserve_policy::bucket)==16,"sixteen actual reserve outputs segregated");
    auto state=db.exchange_state_for_tests();check(state.native_locked==999000 && state.assets.at(native_asset).supply==999000 && state.balances.at({owner.pub,native_asset})==999000,"actual native output commitments produce exact ledger credit");
    check(db.has_key_image(spent_image),"native funding input spent atomically");
    for(unsigned i=0;i<16;++i){auto stored=db.get_output_key(reserve_policy::bucket,i,true);check(stored.commitment==tx.rct_signatures.outPk[i].mask,"reserve native commitment preserved");auto source=db.get_output_tx_and_index(reserve_policy::bucket,i);check(source.first==cryptonote::get_transaction_hash(tx) && source.second==i,"reserve origin index");}
    auto cumulative=db.get_block_cumulative_rct_outputs({80});check(cumulative.size()==1 && cumulative[0]==81,"ordinary cumulative RCT distribution excludes reserve outputs");
    std::vector<uint64_t> distribution;uint64_t base=0;check(db.get_output_distribution(0,0,0,distribution,base) && distribution.back()==81,"ordinary native output distribution excludes reserves");
    auto attack=public_key_attack(db,tx,cfg,owner,0);check(cryptonote::ver_input_proofs_rings(attack,ring(db,reserve_policy::bucket)),"public reserve secret really can sign the reserved ring");
    check(!cryptonote::ver_input_proofs_rings(attack,ring(db,0)),"ordinary bucket lookup rejects reserve-key spend proof");
    attack=public_key_attack(db,tx,cfg,owner,reserve_policy::bucket);check(cryptonote::ver_input_proofs_rings(attack,ring(db,reserve_policy::bucket)),"correct bucket native proof valid before release policy");
    auto attack_block=block(db.top_block_hash(),81,owner.public_key);attack_block.tx_hashes.push_back(cryptonote::get_transaction_hash(attack));
    rejects([&]{append(db,attack_block,{{attack,cryptonote::tx_to_blob(attack)}});},"actual DB append rejects reserve release without authorized adapter");
    check(db.height()==81 && db.get_num_outputs(reserve_policy::bucket)==16 && db.exchange_state_for_tests().native_locked==999000,"blocked release leaves backing intact");
    confirmed=serialize(db.exchange_state_for_tests());db.close();
  }
  {
    cryptonote::BlockchainLMDB db;db.configure_exchange_for_tests(cfg);db.open(temp.path.string());cryptonote::HardFork hf(db,16,0);hf.init();db.set_hard_fork(&hf);
    check(serialize(db.exchange_state_for_tests())==confirmed && db.get_num_outputs(reserve_policy::bucket)==16,"native reserve output and ledger survive restart together");
    cryptonote::block popped;db.pop_block(popped,nullptr);check(db.height()==80 && db.get_num_outputs(0)==80 && db.get_num_outputs(reserve_policy::bucket)==0,"native pop removes segregated reserve and ordinary coinbase outputs");
    check(db.exchange_state_for_tests().native_locked==0 && db.exchange_state_for_tests().assets.empty() && !db.has_key_image(spent_image),"native pop restores exact ledger and funding spendability");
    append(db,candidate,{{tx,cryptonote::tx_to_blob(tx)}});check(serialize(db.exchange_state_for_tests())==confirmed && db.get_num_outputs(reserve_policy::bucket)==16,"native deposit deterministic replay restores identical backing");db.close();
  }
  std::cout<<checks<<" checks passed: native database-funded deposit proofs, protected output routing, exact credit, false-claim rejection, public-key spend isolation, atomic abort/pop/restart/replay\n";
  std::cout<<"Synthetic native block fixtures with valid input/range proofs. No daemon-funded deposit, withdrawal implementation, wallet or public-network activation.\n";
}catch(const std::exception& error){std::cerr<<"FAIL after "<<checks<<" checks: "<<error.what()<<'\n';return 1;}}
