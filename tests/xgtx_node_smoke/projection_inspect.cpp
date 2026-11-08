#include "../../src/xgtx_exchange/engine.hpp"
#include <lmdb.h>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <cstring>
namespace {
void require(int rc){if(rc)throw std::runtime_error(mdb_strerror(rc));}
struct environment {MDB_env* ptr=nullptr;~environment(){if(ptr)mdb_env_close(ptr);}};
struct transaction {MDB_txn* ptr=nullptr;~transaction(){if(ptr)mdb_txn_abort(ptr);}};
}
int main(int argc,char** argv){try{
  if(argc!=2)throw std::invalid_argument("Usage: xgtx_projection_inspect ISOLATED_LMDB_DIRECTORY");
  environment env;require(mdb_env_create(&env.ptr));require(mdb_env_set_maxdbs(env.ptr,64));
  require(mdb_env_open(env.ptr,argv[1],MDB_RDONLY|MDB_NOTLS,0644));
  transaction tx;require(mdb_txn_begin(env.ptr,nullptr,MDB_RDONLY,&tx.ptr));
  MDB_dbi db{};require(mdb_dbi_open(tx.ptr,"xgtx_exchange_state_v1",0,&db));
  const char* key="current";MDB_val k{std::strlen(key),const_cast<char*>(key)},v{};
  require(mdb_get(tx.ptr,db,&k,&v));const auto* p=static_cast<const std::uint8_t*>(v.mv_data);
  auto s=xgtx_exchange::deserialize(xgtx_exchange::bytes(p,p+v.mv_size));
  std::ostringstream tip;tip<<std::hex<<std::setfill('0');for(auto c:s.tip)tip<<std::setw(2)<<static_cast<unsigned>(c);
  std::cout<<"{\"height\":"<<s.height<<",\"tip\":\""<<tip.str()<<"\",\"assets\":"<<s.assets.size()<<",\"balances\":"<<s.balances.size()<<"}\n";
  return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
