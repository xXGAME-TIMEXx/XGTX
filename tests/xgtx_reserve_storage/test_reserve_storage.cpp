#include "../../src/xgtx_exchange/native_chain.hpp"
#include "blockchain_db/lmdb/db_lmdb.h"
#include "../xgtx_exchange/test_native_crypto.hpp"
#include <boost/filesystem.hpp>
#include <iostream>
#include "ringct/rctOps.h"

// Direct record tests, NOT consensus-valid blocks. All native block admission
// and reserve spend guards remain in force; no production entry point added.
namespace cryptonote {
struct xgtx_reserve_storage_test_access {
  static uint64_t add(BlockchainLMDB& db,const crypto::hash& hash,const tx_out& out,
      uint64_t index,const rct::key* commitment){return db.add_output(hash,out,index,0,commitment);}
  static void remove(BlockchainLMDB& db,uint64_t amount,uint64_t index){db.remove_output(amount,index);}
};
}
using namespace xgtx_exchange;
namespace {
unsigned checks=0;
void check(bool value,const char* label){++checks;if(!value)throw std::runtime_error(label);}
template<class F>void rejects(F fn,const char* label){bool failed=false;try{fn();}catch(const std::exception&){failed=true;}check(failed,label);}
struct directory {
  boost::filesystem::path path=boost::filesystem::temp_directory_path()/boost::filesystem::unique_path("xgtx-reserve-storage-%%%%-%%%%-%%%%");
  directory(){boost::filesystem::create_directories(path);}
  ~directory(){boost::system::error_code ec;boost::filesystem::remove_all(path,ec);}
};
template<class F>void write(cryptonote::BlockchainLMDB& db,F fn,bool commit=true){
  db.block_wtxn_start();try{fn();if(commit)db.block_wtxn_stop();else db.block_wtxn_abort();}catch(...){db.block_wtxn_abort();throw;}
}
}
int main(){try{
  directory temp;xgtx_test::wallet owner(1);id chain{};chain.fill(0x39);
  xgtx_exchange::config cfg{chain,owner.pub,2,1024};
  crypto::hash ordinary_hash{},reserve_hash{};ordinary_hash.data[0]=1;reserve_hash.data[0]=2;
  cryptonote::tx_out ordinary;ordinary.amount=0;ordinary.target=cryptonote::txout_to_key(owner.public_key);
  auto reserved=ordinary;reserved.amount=reserve_policy::bucket;
  const auto normal_commitment=rct::commit(100,rct::skGen());
  const auto reserve_commitment=rct::commit(987654321,rct::skGen());
  using access=cryptonote::xgtx_reserve_storage_test_access;
  {
    cryptonote::BlockchainLMDB db;db.configure_exchange_for_tests(cfg);db.open(temp.path.string());
    check(db.get_num_outputs(0)==0 && db.get_num_outputs(reserve_policy::bucket)==0,"fresh segregated buckets");
    write(db,[&]{check(access::add(db,ordinary_hash,ordinary,0,&normal_commitment)==0,"ordinary index zero");});
    write(db,[&]{check(access::add(db,reserve_hash,reserved,7,&reserve_commitment)==0,"reserve index zero in distinct bucket");});
    check(db.get_num_outputs(0)==1,"reserve does not increase ordinary bucket count");
    check(db.get_num_outputs(reserve_policy::bucket)==1,"reserve bucket count");
    auto normal=db.get_output_key(0,0,true);auto stored=db.get_output_key(reserve_policy::bucket,0,true);
    check(normal.commitment==normal_commitment,"ordinary RingCT commitment unchanged");
    check(stored.commitment==reserve_commitment,"reserve commitment is preserved rather than legacy amount commitment");
    check(stored.pubkey==owner.public_key,"reserve public key preserved");
    auto source=db.get_output_tx_and_index(reserve_policy::bucket,0);
    check(source.first==reserve_hash && source.second==7,"reserve origin transaction and local index preserved");
    const uint64_t amounts[]={0,reserve_policy::bucket};std::vector<uint64_t> offsets{0,0};std::vector<cryptonote::output_data_t> results;
    db.get_output_key(epee::span<const uint64_t>(amounts,2),offsets,results,false);
    check(results.size()==2 && results[0].commitment==normal_commitment && results[1].commitment==reserve_commitment,"mixed batch reads preserve both commitments");
    rejects([&]{write(db,[&]{access::add(db,reserve_hash,reserved,8,nullptr);});},"reserve requires explicit commitment");
    check(db.get_num_outputs(reserve_policy::bucket)==1,"missing commitment leaves no record");
    write(db,[&]{access::add(db,reserve_hash,reserved,8,&reserve_commitment);},false);
    check(db.get_num_outputs(reserve_policy::bucket)==1 && db.get_num_outputs(0)==1,"aborted reserve insertion changes neither bucket");
    write(db,[&]{access::remove(db,reserve_policy::bucket,0);},false);
    check(db.get_output_key(reserve_policy::bucket,0,true).commitment==reserve_commitment,"aborted removal restores exact commitment");
    db.close();
  }
  {
    cryptonote::BlockchainLMDB db;db.configure_exchange_for_tests(cfg);db.open(temp.path.string());
    check(db.get_num_outputs(0)==1 && db.get_num_outputs(reserve_policy::bucket)==1,"restart preserves separate bucket counts");
    check(db.get_output_key(reserve_policy::bucket,0,true).commitment==reserve_commitment,"restart preserves native reserve commitment");
    write(db,[&]{access::remove(db,reserve_policy::bucket,0);});
    check(db.get_num_outputs(reserve_policy::bucket)==0,"committed reserve removal");
    check(db.get_num_outputs(0)==1 && db.get_output_key(0,0,true).commitment==normal_commitment,"reserve removal leaves ordinary output intact");
    write(db,[&]{check(access::add(db,reserve_hash,reserved,7,&reserve_commitment)==0,"replay reserve index");});
    check(db.get_output_key(reserve_policy::bucket,0,true).commitment==reserve_commitment,"replay restores exact reserve record");db.close();
  }
  {
    cryptonote::BlockchainLMDB db;db.open(temp.path.string());
    check(db.get_output_key(reserve_policy::bucket,0,true).commitment==reserve_commitment,"read-only interpretation does not synthesize legacy commitment");
    rejects([&]{write(db,[&]{access::add(db,reserve_hash,reserved,8,&reserve_commitment);});},"unconfigured writer cannot create reserve records");
    check(db.get_num_outputs(reserve_policy::bucket)==1,"unconfigured write rejection atomic");db.close();
  }
  std::cout<<checks<<" checks passed: native LMDB reserve commitment records, segregated counts, origin and batch lookup, abort, removal, restart and replay\n";
  std::cout<<"Direct storage fixtures only. No transaction output routing, XGTX deposit credit, redemption or network activation.\n";
}catch(const std::exception& e){std::cerr<<"FAIL after "<<checks<<" checks: "<<e.what()<<'\n';return 1;}}
