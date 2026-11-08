#include "blockchain_db/lmdb/db_lmdb.h"
#include "cryptonote_core/cryptonote_tx_utils.h"
#include "xgtx_exchange/fee_recipient.hpp"
#include "xgtx_exchange/native_chain.hpp"
#include <boost/filesystem.hpp>
#include <charconv>
#include <iostream>
#include <limits>
using namespace cryptonote;
namespace {
std::string hex(const crypto::hash& h){const char digits[]="0123456789abcdef";std::string s;for(auto n:xgtx_exchange::native_id(h)){s+=digits[n>>4];s+=digits[n&15];}return s;}
uint64_t number(const char* s){uint64_t n=0;const std::string text(s);auto r=std::from_chars(text.data(),text.data()+text.size(),n);if(text.empty() || r.ec!=std::errc{} || r.ptr!=text.data()+text.size() || !n)throw std::invalid_argument("invalid positive integer");return n;}
crypto::hash parse_hash(const char* s){const std::string text(s);if(text.size()!=64)throw std::invalid_argument("hash must have 64 lowercase hex characters");crypto::hash h{};auto* p=reinterpret_cast<unsigned char*>(&h);auto digit=[](char c)->unsigned{if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;throw std::invalid_argument("invalid lowercase hex hash");};for(unsigned i=0;i<32;++i)p[i]=(digit(text[i*2])<<4)|digit(text[i*2+1]);return h;}
void inventory(BlockchainLMDB& db){
  if(!db.height())throw std::runtime_error("empty native chain");
  const auto genesis=db.get_block_hash_from_height(0),tip=db.top_block_hash();
  const auto count=db.height(),transactions=db.get_tx_count();const auto commitment=db.migration_native_commitment();
  // One line is consumed by the runner. No credentials or wallet files read.
  std::cout<<"XGTX_MIGRATION_JSON {\"height\":"<<count<<",\"genesis\":\""<<hex(genesis)<<"\",\"tip\":\""<<hex(tip)<<"\",\"native_transactions\":"<<transactions<<",\"native_commitment\":\""<<hex(commitment)<<"\",\"public_activation\":false}"<<std::endl;
}
}
int main(int argc,char** argv){try{
  if(argc!=3 && argc!=7)throw std::invalid_argument("Usage: xgtx-projection-migrate inspect LMDB | rehearse LMDB COUNT GENESIS TIP ACTIVATION_COUNT");
  const std::string mode(argv[1]);if((mode=="inspect" && argc!=3)||(mode=="rehearse" && argc!=7)||(mode!="inspect"&&mode!="rehearse"))throw std::invalid_argument("invalid migration mode/arguments");
  const auto path=boost::filesystem::canonical(argv[2]);
  if(!boost::filesystem::is_regular_file(path/"data.mdb"))throw std::invalid_argument("existing LMDB data file required");
  BlockchainLMDB db;
  if(mode=="inspect") {db.open(path.string(),DBF_RDONLY);inventory(db);db.close();return 0;}
  // Rehearsal writes are confined to a distinctly named local test directory.
  if(path.filename().string()!="lmdb" || path.parent_path().filename().string().find("xgtx-mainnet-migration-test-")!=0)
    throw std::invalid_argument("writes require xgtx-mainnet-migration-test-*/lmdb copy");
  const auto count=number(argv[3]),activation=number(argv[6]);const auto genesis=parse_hash(argv[4]),tip=parse_hash(argv[5]);
  if(count>100000 || activation<=count)throw std::invalid_argument("invalid rehearsal block/activation bounds");
  block compiled_genesis;
  if(!generate_genesis_block(compiled_genesis,get_config(MAINNET).GENESIS_TX,get_config(MAINNET).GENESIS_NONCE) || get_block_hash(compiled_genesis)!=genesis)
    throw std::invalid_argument("snapshot genesis differs from compiled mainnet genesis");
  auto cfg=xgtx_exchange::designated_fee_config(xgtx_exchange::native_id(genesis),activation);cfg.native_backing=true;
  db.configure_exchange_for_tests(cfg);db.open(path.string());
  if(db.height()!=count || db.get_block_hash_from_height(0)!=genesis || db.top_block_hash()!=tip)throw std::runtime_error("snapshot identity changed");
  const auto native_transactions=db.get_tx_count();const auto native_before=db.migration_native_commitment();
  db.rehearse_exchange_migration(count,tip);db.audit_exchange_migration_undo(count,tip);db.close();
  // Reopen and audit again: this exercises persisted journals, not only a cache.
  BlockchainLMDB restart;restart.configure_exchange_for_tests(cfg);restart.open(path.string());restart.initialize_exchange_projection();restart.audit_exchange_migration_undo(count,tip);
  if(restart.migration_native_commitment()!=native_before || restart.get_tx_count()!=native_transactions || restart.height()!=count || restart.top_block_hash()!=tip)throw std::runtime_error("native inventory changed");
  const auto state=restart.exchange_state_for_tests();auto economic=state;economic.height=0;economic.tip={};
  if(!(economic==xgtx_exchange::state{}))throw std::runtime_error("migration economic state is not empty");
  inventory(restart);restart.close();
  std::cout<<"PASS: projection migration, restart, complete aborted undo audit, native inventory preserved; no public activation"<<std::endl;return 0;
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<std::endl;return 1;}}
