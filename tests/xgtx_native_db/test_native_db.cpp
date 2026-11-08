#include "../../src/xgtx_exchange/native_chain.hpp"
#include "blockchain_db/lmdb/db_lmdb.h"
#include "cryptonote_basic/hardfork.h"
#include "../xgtx_exchange/test_native_crypto.hpp"
#include <boost/filesystem.hpp>
#include <iostream>
using namespace xgtx_exchange;
namespace {
int checks=0;
void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
template<class F>void reject(F f,const char* why){bool failed=false;try{f();}catch(const std::exception&){failed=true;}check(failed,why);}
struct directory {
  boost::filesystem::path path=boost::filesystem::temp_directory_path()/boost::filesystem::unique_path("xgtx-db-%%%%-%%%%-%%%%");
  directory(){boost::filesystem::create_directories(path);}
  ~directory(){boost::system::error_code ec;boost::filesystem::remove_all(path,ec);}
};
cryptonote::block make_block(const crypto::hash& parent,std::uint64_t height,const xgtx_test::wallet& miner,std::uint32_t nonce){
  cryptonote::block b;b.major_version=1;b.minor_version=1;b.timestamp=1000+height;b.prev_id=parent;b.nonce=nonce;
  b.miner_tx.version=1;b.miner_tx.unlock_time=height+10;
  cryptonote::txin_gen input;input.height=height;b.miner_tx.vin.push_back(input);
  cryptonote::tx_out output;output.amount=1000;output.target=cryptonote::txout_to_key(miner.public_key);b.miner_tx.vout.push_back(output);
  cryptonote::add_tx_pub_key_to_extra(b.miner_tx,miner.public_key);return b;
}
void append(cryptonote::BlockchainLMDB& db,const cryptonote::block& block,bool commit=true,
    const std::vector<std::pair<cryptonote::transaction,cryptonote::blobdata>>& txs={}){
  db.block_wtxn_start();
  try{
    auto blob=cryptonote::block_to_blob(block);
    db.add_block({block,blob},blob.size(),blob.size(),cryptonote::difficulty_type(1),1000,txs);
    if(commit)db.block_wtxn_stop();else db.block_wtxn_abort();
  }catch(...){db.block_wtxn_abort();throw;}
}
}
int main(){try{
  directory dir;xgtx_test::wallet miner(1);auto genesis=make_block(crypto::null_hash,0,miner,1);
  auto genesis_hash=cryptonote::get_block_hash(genesis);xgtx_exchange::config cfg{native_id(genesis_hash),miner.pub,2,1024};
  auto one=make_block(genesis_hash,1,miner,2);auto one_hash=cryptonote::get_block_hash(one);
  {
    cryptonote::BlockchainLMDB db;db.configure_exchange_for_tests(cfg);db.open(dir.path.string());
    cryptonote::HardFork hf(db,1,0);hf.init();db.set_hard_fork(&hf);
    reject([&]{db.configure_exchange_for_tests(cfg);},"cannot change open database policy");
    append(db,genesis);check(db.height()==1,"native genesis committed");
    check(db.exchange_state_for_tests().height==1,"exchange genesis committed");
    check(db.exchange_state_for_tests().tip==cfg.chain,"genesis tip matches");
    append(db,one,false);check(db.height()==1,"native block abort");
    check(db.exchange_state_for_tests().height==1,"exchange block abort");
    append(db,one);check(db.height()==2,"native child committed");
    check(db.exchange_state_for_tests().tip==native_id(one_hash),"exchange child committed");
    auto snapshot=serialize(db.exchange_state_for_tests());
    auto invalid=make_block(one_hash,2,miner,3);invalid.miner_tx=genesis.miner_tx;
    reject([&]{append(db,invalid);},"native duplicate transaction fails after exchange append");
    check(db.height()==2,"failed native write leaves native height");
    check(serialize(db.exchange_state_for_tests())==snapshot,"failed native write rolls back exchange state and undo");
    // A signed exchange issuance executes first, then duplicate coinbase makes
    // native persistence fail before this unsigned regular fixture is stored.
    cryptonote::transaction carrier;carrier.version=2;
    cryptonote::txin_to_key input;input.key_offsets.assign(16,1);std::memcpy(&input.k_image,miner.pub.data(),32);carrier.vin.push_back(input);
    cryptonote::tx_out output;output.amount=0;output.target=cryptonote::txout_to_key(miner.public_key);carrier.vout.assign(2,output);
    carrier.rct_signatures.type=rct::RCTTypeCLSAG;carrier.rct_signatures.txnFee=1000;
    carrier.rct_signatures.outPk.resize(2);carrier.rct_signatures.ecdhInfo.resize(2);
    for(auto& key:carrier.rct_signatures.outPk)std::memcpy(&key.mask,miner.pub.data(),32);
    cryptonote::add_tx_pub_key_to_extra(carrier,miner.public_key);
    operation issuance;issuance.chain=cfg.chain;issuance.deadline=100;issuance.quantity=1000;issuance.ticker="GAME";
    seal_native_carrier(carrier,miner.sign(issuance),miner.public_key,miner.secret_key);
    auto issue_block=invalid;issue_block.tx_hashes.push_back(cryptonote::get_transaction_prefix_hash(carrier));issue_block.invalidate_hashes();
    std::vector<std::pair<cryptonote::transaction,cryptonote::blobdata>> carrier_txs{{carrier,{}}};
    auto intents=block_intents(issue_block,carrier_txs,engine(cfg,cryptonote_hash,cryptonote_verify));
    check(intents.size()==1,"native block extracts signed carrier");
    engine preview_engine(cfg,cryptonote_hash,cryptonote_verify);auto projected=preview_engine.apply_block(db.exchange_state_for_tests(),3,native_id(one_hash),native_id(cryptonote::get_block_hash(issue_block)),intents);
    check(projected.next.assets.size()==1,"signed issuance succeeds in candidate state");
    reject([&]{append(db,issue_block,true,carrier_txs);},"native write failure after signed asset issuance");
    check(serialize(db.exchange_state_for_tests())==snapshot,"asset supply, balance, nonce and undo roll back with native failure");

    auto forbidden=make_block(one_hash,2,miner,4);auto fields=carrier_fields(forbidden.miner_tx.extra);
    cryptonote::tx_extra_xgtx_exchange field;field.data={1};fields.push_back(field);forbidden.miner_tx.extra=serialize_carrier_fields(fields);
    reject([&]{append(db,forbidden);},"coinbase exchange operation rejected");
    check(db.height()==2 && serialize(db.exchange_state_for_tests())==snapshot,"invalid carrier leaves both ledgers unchanged");
    auto wrong_parent=make_block(genesis_hash,2,miner,5);reject([&]{append(db,wrong_parent);},"wrong exchange parent rejected");
    check(serialize(db.exchange_state_for_tests())==snapshot,"wrong parent atomic");
    cryptonote::block popped;db.pop_block(popped,nullptr);check(db.height()==1,"native pop");
    check(db.exchange_state_for_tests().tip==cfg.chain,"exchange pop restores genesis");
    auto alternative=make_block(genesis_hash,1,miner,22);append(db,alternative);
    check(db.exchange_state_for_tests().tip==native_id(cryptonote::get_block_hash(alternative)),"alternative branch commit");
    db.close();
  }
  {
    cryptonote::BlockchainLMDB db;db.configure_exchange_for_tests(cfg);db.open(dir.path.string());
    cryptonote::HardFork hf(db,1,0);hf.init();db.set_hard_fork(&hf);
    auto tip=db.top_block_hash();check(db.height()==2 && db.exchange_state_for_tests().tip==native_id(tip),"restart projection alignment");
    cryptonote::block popped;db.pop_block(popped,nullptr);check(db.exchange_state_for_tests().tip==cfg.chain,"restart undo persisted");
    append(db,one);check(db.exchange_state_for_tests().tip==native_id(one_hash),"replay original branch after restart");db.close();
  }
  {
    cryptonote::BlockchainLMDB db;db.open(dir.path.string());cryptonote::HardFork hf(db,1,0);hf.init();db.set_hard_fork(&hf);
    auto two=make_block(db.top_block_hash(),2,miner,33);reject([&]{append(db,two);},"unconfigured process cannot advance exchange database");
    cryptonote::block popped;reject([&]{db.pop_block(popped,nullptr);},"unconfigured process cannot pop exchange database");
    check(db.height()==2,"disabled projection fail closed");db.close();
  }
  {
    auto changed=cfg;changed.treasury[0]^=1;cryptonote::BlockchainLMDB db;db.configure_exchange_for_tests(changed);db.open(dir.path.string());
    cryptonote::HardFork hf(db,1,0);hf.init();db.set_hard_fork(&hf);
    auto two=make_block(db.top_block_hash(),2,miner,44);reject([&]{append(db,two);},"restarted policy mismatch");check(db.height()==2,"policy mismatch atomic");db.close();
  }
  std::cout<<checks<<" checks passed: native BlockchainLMDB commit, abort, failed-write rollback, pop, alternative branch, restart, policy guards\n";
  std::cout<<"Synthetic native database fixtures; no PoW/RingCT consensus validation, native XGTX backing or daemon activation.\n";
  return 0;
}catch(const std::exception& e){std::cerr<<"FAIL after "<<checks<<" checks: "<<e.what()<<"\n";return 1;}}
