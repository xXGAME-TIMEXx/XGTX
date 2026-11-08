#include "../../src/xgtx_exchange/native_chain.hpp"
#include "../../src/xgtx_exchange/native_mempool.hpp"
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
struct reserve_input {
  cryptonote::txin_to_key input;
  rct::ctkey secret{};
  rct::ctkeyV members;
  amount quantity=0;
  unsigned real=0;
};
cryptonote::transaction withdraw(cryptonote::BlockchainLMDB& db,const xgtx_exchange::config& cfg,const xgtx_test::wallet& signer,
    const xgtx_test::wallet& payee,const xgtx_test::wallet& second_payee,const std::vector<unsigned>& sources,
    amount nonce,amount change,amount claimed=0,bool wrong_payout=false){
  std::vector<reserve_input> inputs;amount total=0;
  for(auto index:sources){
    auto location=db.get_output_tx_and_index(reserve_policy::bucket,index);auto origin=db.get_pruned_tx(location.first);
    auto carrier=verify_native_carrier(origin,cfg.chain);reserve_proofs::domain domain{cfg.chain,carrier.op.sender,carrier.op.nonce};
    const auto value=carrier.op.type==kind::native_deposit?deposit_part(carrier.op.quantity,carrier.op.limit,location.second):carrier.op.limit;
    const auto opening=reserve_proofs::derive(domain,location.second);auto verified=reserve_proofs::inspect_output(origin,domain,location.second,value);
    reserve_input source;source.quantity=value;source.secret={opening.spend,verified.mask};source.input.amount=reserve_policy::bucket;source.input.k_image=verified.image;
    const unsigned start=index<16?0:index-15;source.real=index-start;source.input.key_offsets.assign(16,1);source.input.key_offsets[0]=start;
    for(unsigned i=0;i<16;++i){auto member=db.get_output_key(reserve_policy::bucket,start+i,true);source.members.push_back({rct::pk2rct(member.pubkey),member.commitment});}
    total+=value;inputs.push_back(std::move(source));
  }
  std::sort(inputs.begin(),inputs.end(),[](const reserve_input& a,const reserve_input& b){return std::memcmp(&a.input.k_image,&b.input.k_image,32)>0;});
  carrier_require(total>change+1000,"test withdrawal requires positive payout");
  const auto net=total-change;operation op;op.type=kind::native_withdraw;op.chain=cfg.chain;op.nonce=nonce;op.deadline=1000;op.quantity=claimed?claimed:net;op.limit=change;
  cryptonote::transaction original;original.version=2;rct::ctkeyV secrets,out_secrets;rct::ctkeyM mix;std::vector<unsigned> indices;std::vector<rct::xmr_amount> inamounts,outamounts;
  for(const auto& source:inputs){original.vin.push_back(source.input);secrets.push_back(source.secret);mix.push_back(source.members);indices.push_back(source.real);inamounts.push_back(source.quantity);}
  rct::keyV destinations,amount_keys;
  if(change){auto key=reserve_proofs::derive({cfg.chain,signer.pub,nonce},0);destinations={rct::pk2rct(key.public_key),rct::pk2rct(payee.public_key)};amount_keys={key.amount_key,rct::skGen()};outamounts={change,net-1000};}
  else {destinations={rct::pk2rct(payee.public_key),rct::pk2rct(second_payee.public_key)};amount_keys={rct::skGen(),rct::skGen()};outamounts={(net-1000)/2,net-1000-(net-1000)/2};}
  for(const auto& key:destinations){cryptonote::tx_out output;output.amount=0;output.target=cryptonote::txout_to_key(rct::rct2pk(key));original.vout.push_back(output);}
  cryptonote::add_tx_pub_key_to_extra(original,signer.public_key);
  op.recipient=wrong_payout?cfg.treasury:withdrawal_payout_binding(original,cfg.chain,change!=0);auto intent=signer.sign(op);
  const auto prefix=cryptonote::get_transaction_prefix_hash(original);auto& device=hw::get_device("default");rct::RCTConfig proof{rct::RangeProofPaddedBulletproof,4};
  auto tx=sign_carrier_transaction(original,intent,signer.public_key,signer.secret_key,
    [&](const std::function<rct::key(const rct::rctSig&)>& finalize){return rct::genRctSimpleWithMessageFinalizer(rct::hash2rct(prefix),secrets,destinations,inamounts,outamounts,1000,mix,amount_keys,indices,out_secrets,proof,device,finalize);});
  check(cryptonote::check_money_overflow(tx),"native overflow rules accept bounded reserve input metadata");
  check(cryptonote::ver_input_proofs_rings(tx,mix),"native reserve input ring proofs");
  check(rct::verRctSemanticsSimple(tx.rct_signatures),"native withdrawal range/conservation proofs");return tx;
}
void legacy_policy_rejects(const xgtx_exchange::config& cfg,const engine& adapter){
  directory temp;MDB_env* raw_env=nullptr;check(mdb_env_create(&raw_env)==0,"legacy policy fixture environment");
  std::unique_ptr<MDB_env,decltype(&mdb_env_close)> env(raw_env,mdb_env_close);
  check(mdb_env_set_maxdbs(env.get(),4)==0 && mdb_env_open(env.get(),temp.path.string().c_str(),0,0600)==0,"legacy policy fixture open");
  MDB_txn* raw_txn=nullptr;check(mdb_txn_begin(env.get(),nullptr,0,&raw_txn)==0,"legacy policy write transaction");
  std::unique_ptr<MDB_txn,decltype(&mdb_txn_abort)> transaction(raw_txn,mdb_txn_abort);MDB_dbi table{};
  check(mdb_dbi_open(transaction.get(),"xgtx_exchange_state_v1",MDB_CREATE,&table)==0,"legacy policy table");
  bytes policy={'X','G','T','X','P','O','L',0,2,1};policy.insert(policy.end(),cfg.chain.begin(),cfg.chain.end());policy.insert(policy.end(),cfg.treasury.begin(),cfg.treasury.end());
  for(auto n:{cfg.activation_height,static_cast<amount>(cfg.max_operations)})for(unsigned j=0;j<8;++j)policy.push_back(static_cast<std::uint8_t>(n>>(8*j)));
  std::string name="policy";MDB_val key{name.size(),name.data()},value{policy.size(),policy.data()};check(mdb_put(transaction.get(),table,&key,&value,0)==0,"legacy reserve policy stored");
  rejects([&]{lmdb_store incompatible(transaction.get(),adapter);},"old max-bucket backing policy rejected rather than silently reinterpreted");
}
cryptonote::block append_tx(cryptonote::BlockchainLMDB& db,const cryptonote::transaction& tx,const crypto::public_key& miner,bool commit=true){
  auto candidate=block(db.top_block_hash(),db.height(),miner);candidate.tx_hashes.push_back(cryptonote::get_transaction_hash(tx));
  append(db,candidate,{{tx,cryptonote::tx_to_blob(tx)}},commit);return candidate;
}
}
int main(){try{
  directory temp;xgtx_test::wallet owner(1),payee(2),treasury(3),intruder(4);std::vector<std::unique_ptr<xgtx_test::wallet>> funding;
  for(unsigned i=0;i<16;++i)funding.emplace_back(new xgtx_test::wallet(i+10));
  auto genesis=block(crypto::null_hash,0,funding[0]->public_key);xgtx_exchange::config cfg{native_id(cryptonote::get_block_hash(genesis)),treasury.pub,1,1024,true};
  engine adapter(cfg,cryptonote_hash,cryptonote_verify);legacy_policy_rejects(cfg,adapter);cryptonote::transaction partial,full;cryptonote::block partial_block,full_block;bytes before_partial,after_partial,after_full;
  {
    cryptonote::BlockchainLMDB db;db.configure_exchange_for_tests(cfg);db.open(temp.path.string());cryptonote::HardFork hf(db,16,0);hf.init();db.set_hard_fork(&hf);
    append(db,genesis);for(unsigned h=1;h<80;++h)append(db,block(db.top_block_hash(),h,h<16?funding[h]->public_key:owner.public_key));
    auto locked=deposit(db,cfg,owner,*funding[0]);
    auto deposit_effect=transaction_effect_from_database(locked,adapter,db);
    auto confirmed_deposit=db.exchange_state_for_tests();auto deposit_intent=*transaction_intent(locked,cfg.chain);
    validate_admission(adapter,confirmed_deposit,deposit_intent,native_id(cryptonote::get_transaction_hash(locked)),{},deposit_effect);
    template_view deposit_template(adapter,confirmed_deposit);
    check(deposit_template.include(locked,deposit_effect) && deposit_template.candidate().native_locked==999000,"deposit admission and template derive native credit from authenticated DB effect");
    check(serialize(confirmed_deposit)==serialize(db.exchange_state_for_tests()),"deposit preview does not commit credit");
    append_tx(db,locked,owner.public_key);
    for(unsigned h=0;h<10;++h)append(db,block(db.top_block_hash(),db.height(),owner.public_key));
    check(db.height()==91 && db.exchange_state_for_tests().native_locked==999000,"mature deposit reserve and ledger backing");
    before_partial=serialize(db.exchange_state_for_tests());const auto normal_before=db.get_num_outputs(0);
    auto unauthorized=withdraw(db,cfg,intruder,payee,treasury,{0},1,42438);
    auto unauthorized_effect=withdrawal_effect_from_database(unauthorized,adapter,db);
    rejects([&]{validate_admission(adapter,db.exchange_state_for_tests(),*transaction_intent(unauthorized,cfg.chain),native_id(cryptonote::get_transaction_hash(unauthorized)),{},unauthorized_effect);},"admission rejects public reserve proof without owner balance");
    check(unauthorized_effect.released==62438 && unauthorized_effect.locked==42438,"public reserve keys alone can produce native proofs");
    rejects([&]{append_tx(db,unauthorized,owner.public_key);},"native proof cannot withdraw without signed owner's ledger balance");
    check(serialize(db.exchange_state_for_tests())==before_partial && db.get_num_outputs(reserve_policy::bucket)==16,"unauthorized withdrawal leaves both ledgers intact");
    auto incorrect=withdraw(db,cfg,owner,payee,treasury,{0},2,42438,20001);
    rejects([&]{append_tx(db,incorrect,owner.public_key);},"signed debit differing from actual reserve decrease rejects");
    incorrect=withdraw(db,cfg,owner,payee,treasury,{0},2,42438,0,true);
    rejects([&]{append_tx(db,incorrect,owner.public_key);},"signed payout digest differing from actual keys rejects");
    partial=withdraw(db,cfg,owner,payee,treasury,{0},2,42438);
    auto partial_effect=withdrawal_effect_from_database(partial,adapter,db);check(partial_effect.released==62438 && partial_effect.locked==42438,"database reserve origin determines exact net debit");
    auto partial_intent=*transaction_intent(partial,cfg.chain);auto partial_id=native_id(cryptonote::get_transaction_hash(partial));
    validate_admission(adapter,db.exchange_state_for_tests(),partial_intent,partial_id,{},transaction_effect_from_database(partial,adapter,db));
    id other_id=partial_id;other_id[0]^=1;
    rejects([&]{validate_admission(adapter,db.exchange_state_for_tests(),partial_intent,partial_id,{{other_id,partial_intent.op}},partial_effect);},"pending withdrawal reserves owner nonce despite different native input");
    template_view withdrawal_template(adapter,db.exchange_state_for_tests());
    check(withdrawal_template.include(partial,partial_effect) && withdrawal_template.candidate().native_locked==979000,"withdrawal mining preview debits native backing");
    auto preview=serialize(withdrawal_template.candidate());
    rejects([&]{withdrawal_template.include(partial,partial_effect);},"template rejects repeated withdrawal nonce");
    check(serialize(withdrawal_template.candidate())==preview && withdrawal_template.operations()==1,"failed template candidate preserves prior state and operation count");
    check(serialize(db.exchange_state_for_tests())==before_partial,"admission and template never mutate confirmed balance");
    append_tx(db,partial,owner.public_key,false);check(serialize(db.exchange_state_for_tests())==before_partial && db.get_num_outputs(reserve_policy::bucket)==16,"aborted withdrawal restores reserve and ledger");
    partial_block=append_tx(db,partial,owner.public_key);auto state=db.exchange_state_for_tests();
    check(state.native_locked==979000 && state.balances.at({owner.pub,native_asset})==979000 && state.nonces.at(owner.pub)==2,"native payout debits owner and backing exactly including mining fee");
    check(db.get_num_outputs(reserve_policy::bucket)==17 && db.get_num_outputs(0)==normal_before+2,"reserve change isolated and payout enters ordinary bucket");
    check(db.has_key_image(boost::get<cryptonote::txin_to_key>(partial.vin[0]).k_image),"reserve input consumed atomically");
    check(db.get_output_key(reserve_policy::bucket,16,true).commitment==partial.rct_signatures.outPk[0].mask,"actual native reserve change commitment stored");
    auto origin=db.get_output_tx_and_index(reserve_policy::bucket,16);check(origin.first==cryptonote::get_transaction_hash(partial) && origin.second==0,"native change origin metadata");
    after_partial=serialize(state);
    rejects([&]{withdrawal_effect_from_database(partial,adapter,db);},"native key image replay rejected");
    db.close();
  }
  {
    cryptonote::BlockchainLMDB db;db.configure_exchange_for_tests(cfg);db.open(temp.path.string());cryptonote::HardFork hf(db,16,0);hf.init();db.set_hard_fork(&hf);
    check(serialize(db.exchange_state_for_tests())==after_partial && db.get_num_outputs(reserve_policy::bucket)==17,"withdrawal and reserve change survive restart");
    std::vector<unsigned> inputs;for(unsigned i=1;i<=16;++i)inputs.push_back(i);
    full=withdraw(db,cfg,owner,payee,treasury,inputs,3,0);
    check(full.vin.size()==16 && cryptonote::check_money_overflow(full),"full withdrawal keeps native overflow protection with sixteen inputs");
    auto release=withdrawal_effect_from_database(full,adapter,db);check(release.released==979000 && release.locked==0,"origin resolution includes prior withdrawal change and retained pruned transaction bases");
    full_block=append_tx(db,full,owner.public_key);auto state=db.exchange_state_for_tests();
    check(state.native_locked==0 && state.assets.at(native_asset).supply==0 && !state.balances.count({owner.pub,native_asset}),"full withdrawal leaves no unbacked native balance or trapped reserve dust");
    check(db.get_num_outputs(reserve_policy::bucket)==17,"spent reserve records retained as ring decoys");after_full=serialize(state);db.close();
  }
  {
    cryptonote::BlockchainLMDB db;db.configure_exchange_for_tests(cfg);db.open(temp.path.string());cryptonote::HardFork hf(db,16,0);hf.init();db.set_hard_fork(&hf);
    check(serialize(db.exchange_state_for_tests())==after_full,"full redemption persists through restart");
    cryptonote::block popped;db.pop_block(popped,nullptr);check(serialize(db.exchange_state_for_tests())==after_partial && db.get_num_outputs(reserve_policy::bucket)==17,"full withdrawal rollback restores exact claims and reserve spendability");
    db.pop_block(popped,nullptr);check(serialize(db.exchange_state_for_tests())==before_partial && db.get_num_outputs(reserve_policy::bucket)==16,"partial withdrawal rollback removes change and restores owner balance");
    append(db,partial_block,{{partial,cryptonote::tx_to_blob(partial)}});check(serialize(db.exchange_state_for_tests())==after_partial,"partial withdrawal replay identical");
    append(db,full_block,{{full,cryptonote::tx_to_blob(full)}});check(serialize(db.exchange_state_for_tests())==after_full,"full withdrawal replay identical");db.close();
  }
  std::cout<<checks<<" checks passed: native reserve origin authentication, signed payout/debit, public-key theft rejection, partial/change/full release, overflow compatibility, atomic storage, restart/pop/replay\n";
  std::cout<<"Synthetic native DB blocks with valid native proofs. Admission/template adapters exercised with native DB fixtures; running-daemon funded bridge, wallet and public activation remain unverified.\n";
}catch(const std::exception& error){std::cerr<<"FAIL after "<<checks<<" checks: "<<error.what()<<'\n';return 1;}}
