#include "xgtx_exchange/native_chain.hpp"
#include "blockchain_db/lmdb/db_lmdb.h"
#include "cryptonote_basic/hardfork.h"
#include <boost/filesystem.hpp>
#include <iostream>
using namespace cryptonote;
namespace {
unsigned checks=0;
void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
template<class F>void reject(F f,const char* why){bool failed=false;try{f();}catch(const std::exception&){failed=true;}check(failed,why);}
struct directory {boost::filesystem::path path=boost::filesystem::temp_directory_path()/boost::filesystem::unique_path("xgtx-mainnet-migration-test-%%%%-%%%%");directory(){boost::filesystem::create_directories(path);}~directory(){boost::system::error_code ec;boost::filesystem::remove_all(path,ec);}};
block make_block(const crypto::hash& parent,uint64_t height,const crypto::public_key& miner){
 block b;b.major_version=15;b.minor_version=15;b.timestamp=1000+height;b.prev_id=parent;b.nonce=height+1;
 b.miner_tx.version=1;b.miner_tx.unlock_time=height+60;txin_gen input;input.height=height;b.miner_tx.vin.push_back(input);
 tx_out out;out.amount=1000;out.target=txout_to_key(miner);b.miner_tx.vout.push_back(out);add_tx_pub_key_to_extra(b.miner_tx,miner);return b;
}
void append(BlockchainLMDB& db,const block& b){db.block_wtxn_start();try{auto blob=block_to_blob(b);db.add_block({b,blob},blob.size(),blob.size(),difficulty_type(1),1000,{});db.block_wtxn_stop();}catch(...){db.block_wtxn_abort();throw;}}
}
int main(){try{
 directory dir;crypto::public_key pub{};crypto::secret_key secret{};crypto::generate_keys(pub,secret);
 auto genesis=make_block(crypto::null_hash,0,pub),one=make_block(get_block_hash(genesis),1,pub),two=make_block(get_block_hash(one),2,pub);auto tip=get_block_hash(two);
 {BlockchainLMDB db;db.open(dir.path.string());HardFork hf(db,15,0);hf.init();db.set_hard_fork(&hf);append(db,genesis);append(db,one);append(db,two);db.close();}
 auto cfg=xgtx_exchange::config{xgtx_exchange::native_id(get_block_hash(genesis)),xgtx_exchange::native_id(tip),4,1024,true};
 {BlockchainLMDB db;db.configure_exchange_for_tests(cfg);db.open(dir.path.string());
 reject([&]{db.rehearse_exchange_migration(2,tip);},"wrong expected count rejected");
 auto wrong=tip;reinterpret_cast<unsigned char*>(&wrong)[0]^=1;reject([&]{db.rehearse_exchange_migration(3,wrong);},"wrong expected tip rejected");
 db.rehearse_exchange_migration(3,tip);check(db.height()==3 && db.top_block_hash()==tip,"native chain preserved");check(db.exchange_state_for_tests().height==3,"projection aligns");
 check(db.get_tx_count()==3,"native coinbase count preserved");db.audit_exchange_migration_undo(3,tip);check(db.exchange_state_for_tests().height==3,"audit abort preserves state");
 reject([&]{db.rehearse_exchange_migration(3,tip);},"repeat migration refused");db.close();}
 {BlockchainLMDB db;db.configure_exchange_for_tests(cfg);db.open(dir.path.string());HardFork hf(db,15,0);hf.init();db.set_hard_fork(&hf);db.initialize_exchange_projection();db.audit_exchange_migration_undo(3,tip);check(db.exchange_state_for_tests().tip==xgtx_exchange::native_id(tip),"restart audit persists");
 block popped;db.pop_block(popped,nullptr);check(db.height()==2 && db.exchange_state_for_tests().height==2,"native pop uses migrated undo");append(db,two);check(db.top_block_hash()==tip && db.exchange_state_for_tests().height==3,"native replay after pop");db.close();}
 {auto changed=cfg;changed.activation_height=5;BlockchainLMDB db;db.configure_exchange_for_tests(changed);db.open(dir.path.string());reject([&]{db.initialize_exchange_projection();},"policy mismatch fails closed");db.close();}
 {directory invalid;auto bad=genesis;tx_extra_xgtx_exchange field;field.data={1};auto fields=xgtx_exchange::carrier_fields(bad.miner_tx.extra);fields.push_back(field);bad.miner_tx.extra=xgtx_exchange::serialize_carrier_fields(fields);bad.invalidate_hashes();
 {BlockchainLMDB db;db.open(invalid.path.string());HardFork hf(db,15,0);hf.init();db.set_hard_fork(&hf);append(db,bad);db.close();}
 auto c=cfg;c.chain=xgtx_exchange::native_id(get_block_hash(bad));BlockchainLMDB db;db.configure_exchange_for_tests(c);db.open(invalid.path.string());reject([&]{db.rehearse_exchange_migration(1,get_block_hash(bad));},"historical carrier rejected");check(db.height()==1,"failed migration leaves native history intact");db.close();}
 std::cout<<checks<<" checks passed: native DB migration, mismatch guards, restart, aborted full undo audit, actual pop/replay and historical carrier refusal\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL after "<<checks<<" checks: "<<e.what()<<'\n';return 1;}}
