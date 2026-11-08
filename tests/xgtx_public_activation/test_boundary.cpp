#include "xgtx_exchange/activation_policy.hpp"
#include "xgtx_exchange/native_chain.hpp"
#include "blockchain_db/lmdb/db_lmdb.h"
#include "cryptonote_basic/hardfork.h"
#include "hardforks/hardforks.h"
#include <boost/filesystem.hpp>
#include <iostream>
using namespace cryptonote;
namespace {
unsigned checks=0;void check(bool b,const char* s){++checks;if(!b)throw std::runtime_error(s);}
template<class F>void reject(F f,const char* s){bool bad=false;try{f();}catch(const std::exception&){bad=true;}check(bad,s);}
block make(const crypto::hash& parent,uint64_t h,const crypto::public_key& miner,uint8_t version){block b;b.major_version=version;b.minor_version=version;b.prev_id=parent;b.timestamp=1000+h;b.nonce=h+1;b.miner_tx.version=1;b.miner_tx.unlock_time=h+60;txin_gen input;input.height=h;b.miner_tx.vin.push_back(input);tx_out o;o.amount=1000;o.target=txout_to_key(miner);b.miner_tx.vout.push_back(o);add_tx_pub_key_to_extra(b.miner_tx,miner);return b;}
void append(BlockchainLMDB& db,const block& b){db.block_wtxn_start();try{auto blob=block_to_blob(b);db.add_block({b,blob},blob.size(),blob.size(),difficulty_type(1),1000,{});db.block_wtxn_stop();}catch(...){db.block_wtxn_abort();throw;}}
void opaque(block& b){auto fields=xgtx_exchange::carrier_fields(b.miner_tx.extra);tx_extra_xgtx_exchange f;f.data={1};fields.push_back(f);b.miner_tx.extra=xgtx_exchange::serialize_carrier_fields(fields);b.invalidate_hashes();}
}
int main(){auto path=boost::filesystem::temp_directory_path()/boost::filesystem::unique_path("xgtx-public-boundary-%%%%-%%%%");try{
 check(xgtx_exchange::public_candidate_compiled,"candidate build explicitly enabled");
 check(mainnet_hard_forks[num_mainnet_hard_forks-1].version==16 && mainnet_hard_forks[num_mainnet_hard_forks-1].height==700,"mainnet fixed fork schedule");
 check(testnet_hard_forks[num_testnet_hard_forks-1].version==15 && stagenet_hard_forks[num_stagenet_hard_forks-1].version==15,"other public networks unchanged");
 crypto::public_key pub{};crypto::secret_key secret{};crypto::generate_keys(pub,secret);
 auto genesis=make(crypto::null_hash,0,pub,15),one=make(get_block_hash(genesis),1,pub,15);opaque(one);auto two=make(get_block_hash(one),2,pub,15);
 auto cfg=xgtx_exchange::config{xgtx_exchange::native_id(get_block_hash(genesis)),xgtx_exchange::native_id(get_block_hash(two)),4,1024,true};
 {BlockchainLMDB db;db.configure_exchange_for_tests(cfg);db.open(path.string());HardFork hf(db,15,0);check(hf.add_fork(15,0,0,1),"initial hard fork configured");check(hf.add_fork(16,3,0,2),"boundary hard fork configured");hf.init();db.set_hard_fork(&hf);
 append(db,genesis);append(db,one);append(db,two);check(db.exchange_state_for_tests().height==3 && db.exchange_state_for_tests().assets.empty(),"legacy opaque carrier before fork has no economic effect");
 auto old=make(get_block_hash(two),3,pub,15),active=make(get_block_hash(two),3,pub,16);check(!hf.check(old) && hf.check(active),"native hard fork rejects old major version at boundary");
 auto malformed=active;opaque(malformed);reject([&]{append(db,malformed);},"coinbase carrier rejected after activation");check(db.height()==3 && db.exchange_state_for_tests().height==3,"active invalid block aborts both histories");append(db,active);check(db.height()==4 && db.exchange_state_for_tests().height==4,"first active native block projects");
 block popped;db.pop_block(popped,nullptr);hf.on_block_popped(1);check(db.height()==3 && db.exchange_state_for_tests().height==3,"rollback across activation aligns projection");
 db.pop_block(popped,nullptr);hf.on_block_popped(1);db.pop_block(popped,nullptr);hf.on_block_popped(1);check(db.height()==1,"rollback removes opaque pre-fork transaction without reserve classification");append(db,one);append(db,two);append(db,active);check(db.exchange_state_for_tests().height==4,"deterministic replay across boundary");db.close();}
 {BlockchainLMDB db;db.configure_exchange_for_tests(cfg);db.open(path.string());db.initialize_exchange_projection();check(db.height()==4 && db.exchange_state_for_tests().height==4,"restart after boundary preserves both ledgers");db.close();}
 boost::filesystem::remove_all(path);std::cout<<checks<<" checks passed: candidate fork schedule, legacy handling, native version boundary, atomic failure, pop/replay and restart\n";return 0;
}catch(const std::exception& e){boost::system::error_code ec;boost::filesystem::remove_all(path,ec);std::cerr<<"FAIL after "<<checks<<" checks: "<<e.what()<<'\n';return 1;}}
