#include "../../src/xgtx_exchange/projection_migration.hpp"
#include "../xgtx_exchange/test_crypto.hpp"
#include <filesystem>
#include <iostream>
#include <unistd.h>
using namespace xgtx_exchange;
namespace {
unsigned checks=0;
void check(bool yes,const char* why){++checks;if(!yes)throw std::runtime_error(why);}
template<class F>void reject(F f,const char* why){bool failed=false;try{f();}catch(const std::exception&){failed=true;}check(failed,why);}
id key(amount n){id k{};for(unsigned i=0;i<8;++i)k[i]=n>>(i*8);return k;}
struct database {
 MDB_env* env=nullptr;std::filesystem::path path;
 database(){char name[]="/tmp/xgtx-migration-XXXXXX";auto p=mkdtemp(name);if(!p)throw std::runtime_error("mkdtemp");path=p;open();}
 void open(){if(mdb_env_create(&env)||mdb_env_set_maxdbs(env,8)||mdb_env_set_mapsize(env,128*1024*1024)||mdb_env_open(env,path.c_str(),0,0600))throw std::runtime_error("LMDB open");}
 void reopen(){mdb_env_close(env);open();}
 ~database(){if(env)mdb_env_close(env);std::filesystem::remove_all(path);}
 MDB_txn* begin(){MDB_txn* txn=nullptr;if(mdb_txn_begin(env,nullptr,0,&txn))throw std::runtime_error("begin");return txn;}
 void commit(MDB_txn* txn){if(mdb_txn_commit(txn))throw std::runtime_error("commit");}
};
void corrupt_undo(MDB_txn* t,amount h){MDB_dbi d{};if(mdb_dbi_open(t,"xgtx_exchange_state_v1",0,&d))throw std::runtime_error("open");std::string key="undo/";for(unsigned i=0;i<8;++i)key+=static_cast<char>(h>>(i*8));MDB_val k{key.size(),key.data()},v{};if(mdb_get(t,d,&k,&v))throw std::runtime_error("get");auto p=static_cast<const uint8_t*>(v.mv_data);bytes b(p,p+v.mv_size);b.back()^=1;v={b.size(),b.data()};if(mdb_put(t,d,&k,&v,0))throw std::runtime_error("put");}
}
int main(){try{
 constexpr amount count=642;config cfg{key(1),key(9000),count+1,1024,true};engine e(cfg,xgtx_test::hash,xgtx_test::verify);
 auto fetch=[](amount n){return migration_block{key(n+1),n?key(n):id{},true};};auto hash_at=[](amount n){return key(n+1);};
 auto failure=[&](auto run,const char* why){database d;auto t=d.begin();reject([&]{run(t);},why);mdb_txn_abort(t);t=d.begin();check(!projection_table_exists(t),"failed migration leaves no table after abort");mdb_txn_abort(t);};
 failure([&](MDB_txn* t){stage_pre_activation_projection(t,e,0,key(count),fetch);},"zero count rejected");
 failure([&](MDB_txn* t){stage_pre_activation_projection(t,e,100001,key(count),fetch);},"unbounded count rejected");
 failure([&](MDB_txn* t){stage_pre_activation_projection(t,e,count,key(999),fetch);},"wrong tip rejected atomically after staging");
 failure([&](MDB_txn* t){auto c=cfg;c.chain=key(8);engine wrong(c,xgtx_test::hash,xgtx_test::verify);stage_pre_activation_projection(t,wrong,count,key(count),fetch);},"wrong genesis rejected");
 failure([&](MDB_txn* t){auto c=cfg;c.activation_height=count;engine wrong(c,xgtx_test::hash,xgtx_test::verify);stage_pre_activation_projection(t,wrong,count,key(count),fetch);},"activation at current count rejected");
 failure([&](MDB_txn* t){stage_pre_activation_projection(t,e,count,key(count),[&](amount n){auto b=fetch(n);if(n==300)b.parent=key(99);return b;});},"broken historical linkage rejected");
 failure([&](MDB_txn* t){stage_pre_activation_projection(t,e,count,key(count),[&](amount n){auto b=fetch(n);if(n==300)b.ordinary_history=false;return b;});},"ambiguous historic economic data rejected");
 failure([&](MDB_txn* t){stage_pre_activation_projection(t,e,count,key(count),[&](amount n){if(n==300)throw std::runtime_error("missing block");return fetch(n);});},"missing historical block rejected");
 database db;auto t=db.begin();
 MDB_dbi native{};if(mdb_dbi_open(t,"native_fixture",MDB_CREATE|MDB_DUPSORT,&native))throw std::runtime_error("fixture table");
 std::string nk="balance",nv="1000";MDB_val native_key{nk.size(),nk.data()},native_value{nv.size(),nv.data()};if(mdb_put(t,native,&native_key,&native_value,0))throw std::runtime_error("fixture write");
 nv="2000";native_value={nv.size(),nv.data()};if(mdb_put(t,native,&native_key,&native_value,0))throw std::runtime_error("duplicate fixture write");
 const auto native_before=native_tables_migration_commitment(t,xgtx_test::hash);db.commit(t);t=db.begin();
 auto migrated=stage_pre_activation_projection(t,e,count,key(count),fetch);
 auto empty=migrated;empty.height=0;empty.tip={};check(empty==state{},"no economic credits fabricated");check(migrated.height==count && migrated.tip==key(count),"642-block history aligned");
 check(native_tables_migration_commitment(t,xgtx_test::hash)==native_before,"native table commitment excludes only new projection");db.commit(t);db.reopen();
 t=db.begin();lmdb_store persisted(t,e);check(persisted.load(t)==migrated,"state persists after reopen");
 check(native_tables_migration_commitment(t,xgtx_test::hash)==native_before,"native commitment survives restart");mdb_txn_abort(t);
 t=db.begin();MDB_dbi reopened{};if(mdb_dbi_open(t,"native_fixture",0,&reopened))throw std::runtime_error("reopen fixture");
 nv="3000";native_value={nv.size(),nv.data()};if(mdb_put(t,reopened,&native_key,&native_value,0))throw std::runtime_error("change fixture");
 check(native_tables_migration_commitment(t,xgtx_test::hash)!=native_before,"every duplicate native value is committed");mdb_txn_abort(t);
 t=db.begin();check(native_tables_migration_commitment(t,xgtx_test::hash)==native_before,"native mutation abort restores commitment");
 MDB_dbi similar{};if(mdb_dbi_open(t,"xgtx_exchange_state_v1_unrelated",MDB_CREATE,&similar))throw std::runtime_error("similar table");
 check(native_tables_migration_commitment(t,xgtx_test::hash)!=native_before,"similarly named native table not excluded");mdb_txn_abort(t);
 t=db.begin();reject([&]{stage_pre_activation_projection(t,e,count,key(count),fetch);},"existing projection never overwritten");mdb_txn_abort(t);
 t=db.begin();audit_pre_activation_projection_undo(t,e,count,key(count),hash_at);check(lmdb_store(t,e).load(t)==state{},"all undo records restore empty origin");mdb_txn_abort(t);
 t=db.begin();check(lmdb_store(t,e).load(t)==migrated,"aborted undo audit preserves persisted state");mdb_txn_abort(t);
 t=db.begin();corrupt_undo(t,count-2);reject([&]{audit_pre_activation_projection_undo(t,e,count,key(count),hash_at);},"corrupt persisted undo fails audit");mdb_txn_abort(t);
 t=db.begin();audit_pre_activation_projection_undo(t,e,count,key(count),hash_at);mdb_txn_abort(t);check(true,"abort restores even partially processed audit journals");
 t=db.begin();lmdb_store store(t,e);store.pop(t,key(count));store.append(t,count,key(count-1),key(10000),{});check(store.load(t).tip==key(10000),"migrated history can roll back and append alternate branch");mdb_txn_abort(t);
 t=db.begin();reject([&]{audit_pre_activation_projection_undo(t,e,count,key(19),hash_at);},"audit wrong tip rejected");mdb_txn_abort(t);
 reject([&]{stage_pre_activation_projection(nullptr,e,count,key(count),fetch);},"null transaction rejected");
 std::cout<<checks<<" checks passed: 642-block migration, identity/linkage/activation guards, atomic failure, restart, full undo audit, corruption and alternate branch\n";
 return 0;
}catch(const std::exception& e){std::cerr<<"FAIL after "<<checks<<" checks: "<<e.what()<<'\n';return 1;}}
