#include "../../src/xgtx_exchange/native_signing.hpp"
#include "cryptonote_core/cryptonote_tx_utils.h"
#include "cryptonote_core/tx_verification_utils.h"
#include "cryptonote_basic/account.h"
#include "cryptonote_config.h"
#include "string_tools.h"
#include <array>
#include <lmdb.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <fcntl.h>
#include <unistd.h>
namespace {
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
std::string hex(const void* p,std::size_t n){auto b=static_cast<const unsigned char*>(p);std::ostringstream s;s<<std::hex<<std::setfill('0');for(std::size_t i=0;i<n;++i)s<<std::setw(2)<<unsigned(b[i]);return s.str();}
void read(std::istream& in,void* p,std::size_t n){require(bool(in.read(static_cast<char*>(p),n)),"truncated fixture data");}
std::uint64_t number(std::istream& in,unsigned n){std::uint64_t value=0;for(unsigned i=0;i<n;++i){unsigned char b=0;read(in,&b,1);value|=std::uint64_t(b)<<(8*i);}return value;}
void inspect(const char* path){
  struct env_guard{MDB_env* ptr=nullptr;~env_guard(){if(ptr)mdb_env_close(ptr);}} env;
  struct tx_guard{MDB_txn* ptr=nullptr;~tx_guard(){if(ptr)mdb_txn_abort(ptr);}} tx;
  auto ok=[](int rc){if(rc)throw std::runtime_error(mdb_strerror(rc));};
  ok(mdb_env_create(&env.ptr));ok(mdb_env_set_maxdbs(env.ptr,64));ok(mdb_env_open(env.ptr,path,MDB_RDONLY|MDB_NOTLS,0644));ok(mdb_txn_begin(env.ptr,nullptr,MDB_RDONLY,&tx.ptr));
  MDB_dbi db{};ok(mdb_dbi_open(tx.ptr,"xgtx_exchange_state_v1",0,&db));std::string key="current";MDB_val k{key.size(),const_cast<char*>(key.data())},v{};ok(mdb_get(tx.ptr,db,&k,&v));
  auto data=static_cast<const std::uint8_t*>(v.mv_data);auto state=xgtx_exchange::deserialize(xgtx_exchange::bytes(data,data+v.mv_size));
  require(state.assets.size()<=32 && state.balances.size()<=256 && state.lp.size()<=256 && state.pools.size()<=32 && state.nonces.size()<=64,"fixture inspector row limit");
  auto idhex=[](const xgtx_exchange::id& id){return hex(id.data(),id.size());};
  std::cout<<"{\"height\":"<<state.height<<",\"tip\":\""<<idhex(state.tip)<<"\",\"assets\":[";
  bool comma=false;for(const auto& entry:state.assets){if(comma)std::cout<<',';comma=true;const auto& a=entry.second;std::cout<<"{\"id\":\""<<idhex(entry.first)<<"\",\"issuer\":\""<<idhex(a.issuer)<<"\",\"supply\":"<<a.supply<<",\"ticker\":\""<<a.ticker<<"\"}";}
  std::cout<<"],\"balances\":[";comma=false;for(const auto& entry:state.balances){if(comma)std::cout<<',';comma=true;std::cout<<"{\"owner\":\""<<idhex(entry.first.first)<<"\",\"asset\":\""<<idhex(entry.first.second)<<"\",\"amount\":"<<entry.second<<'}';}
  std::cout<<"],\"lp\":[";comma=false;for(const auto& entry:state.lp){if(comma)std::cout<<',';comma=true;std::cout<<"{\"owner\":\""<<idhex(entry.first.first)<<"\",\"pool\":\""<<idhex(entry.first.second)<<"\",\"amount\":"<<entry.second<<'}';}
  std::cout<<"],\"pools\":[";comma=false;for(const auto& entry:state.pools){if(comma)std::cout<<',';comma=true;const auto& p=entry.second;std::cout<<"{\"id\":\""<<idhex(entry.first)<<"\",\"a\":\""<<idhex(p.a)<<"\",\"b\":\""<<idhex(p.b)<<"\",\"reserve_a\":"<<p.reserve_a<<",\"reserve_b\":"<<p.reserve_b<<",\"shares\":"<<p.shares<<'}';}
  std::cout<<"],\"nonces\":[";comma=false;for(const auto& entry:state.nonces){if(comma)std::cout<<',';comma=true;std::cout<<"{\"owner\":\""<<idhex(entry.first)<<"\",\"nonce\":"<<entry.second<<'}';}std::cout<<"]}\n";
}
void store_wallet(const char* path,const cryptonote::account_base& wallet){
  int fd=::open(path,O_WRONLY|O_CREAT|O_EXCL,0600);require(fd>=0,"cannot create new temporary fixture key file");
  std::array<unsigned char,74> data{};std::memcpy(data.data(),"XGTXTESTK1",10);
  std::memcpy(data.data()+10,&wallet.get_keys().m_spend_secret_key,32);std::memcpy(data.data()+42,&wallet.get_keys().m_view_secret_key,32);
  std::size_t done=0;while(done<data.size()){auto n=::write(fd,data.data()+done,data.size()-done);if(n<=0){::close(fd);throw std::runtime_error("fixture key write failed");}done+=n;}
  require(::close(fd)==0,"fixture key close failed");
}
void load_wallet(const char* path,cryptonote::account_base& wallet){
  std::ifstream in(path,std::ios::binary);char magic[10];read(in,magic,10);require(std::memcmp(magic,"XGTXTESTK1",10)==0,"not an ephemeral XGTX fixture key file");
  crypto::secret_key spend{},view{};read(in,&spend,32);read(in,&view,32);require(in.peek()==std::char_traits<char>::eof(),"fixture key size");
  cryptonote::account_public_address address{};require(crypto::secret_key_to_public_key(spend,address.m_spend_public_key),"fixture spend key");require(crypto::secret_key_to_public_key(view,address.m_view_public_key),"fixture view key");wallet.create_from_keys(address,spend,view);
}
}
int main(int argc,char** argv){try{
  require(argc>=3,"Usage: xgtx_funded_builder create KEYFILE | build KEYFILE FUNDING POSITION TICKER");
  if(std::string(argv[1])=="inspect"){require(argc==3,"inspect argument count");inspect(argv[2]);return 0;}
  if(std::string(argv[1])=="create"){
    require(argc==3,"create argument count");cryptonote::account_base wallet;wallet.generate();store_wallet(argv[2],wallet);
    std::cout<<"{\"address\":\""<<cryptonote::get_account_address_as_str(cryptonote::MAINNET,false,wallet.get_keys().m_account_address)<<"\",\"owner\":\""<<hex(&wallet.get_keys().m_account_address.m_spend_public_key,32)<<"\"}\n";return 0;
  }
  require((std::string(argv[1])=="build" || std::string(argv[1])=="build-operation") && argc==6,"build argument count");
  cryptonote::account_base wallet;load_wallet(argv[2],wallet);std::ifstream input(argv[3],std::ios::binary);
  char magic[9];read(input,magic,9);require(std::memcmp(magic,"XGTXFUND1",9)==0,"funding fixture domain");
  xgtx_exchange::id chain{};read(input,chain.data(),32);auto deadline=number(input,8);auto fee=number(input,8);require(fee==COIN,"fixture fee must be one regression coin");auto count=number(input,4);
  require(count==16,"fixture must have exactly 16 native ring members");std::vector<cryptonote::block> blocks;std::vector<std::uint64_t> indices;
  for(unsigned i=0;i<count;++i){indices.push_back(number(input,8));auto n=number(input,4);require(n>0 && n<1024*1024,"fixture block size");cryptonote::blobdata blob(n,'\0');read(input,blob.data(),n);cryptonote::block b;require(cryptonote::parse_and_validate_block_from_blob(cryptonote::blobdata_ref{blob.data(),blob.size()},b),"funding block parse");require(b.miner_tx.version==2 && b.miner_tx.vout.size()==1,"fixture requires single-output modern coinbase");blocks.push_back(b);}
  require(input.peek()==std::char_traits<char>::eof(),"funding trailing bytes");
  unsigned position=std::stoul(argv[4]);require(position<count,"real input position");
  cryptonote::tx_source_entry source{};source.real_output=position;source.real_output_in_tx_index=0;source.amount=blocks[position].miner_tx.vout[0].amount;source.rct=true;source.mask=rct::identity();source.real_out_tx_key=cryptonote::get_tx_pub_key_from_extra(blocks[position].miner_tx);
  for(unsigned i=0;i<count;++i){if(i)require(indices[i]>indices[i-1],"ring indices must increase");crypto::public_key key{};require(cryptonote::get_output_public_key(blocks[i].miner_tx.vout[0],key),"funding output public key");source.push_output(indices[i],key,blocks[i].miner_tx.vout[0].amount);}
  require(source.amount>fee+COIN,"insufficient regression funding");
  cryptonote::account_base recipient;recipient.generate();std::vector<cryptonote::tx_destination_entry> destinations{{COIN,recipient.get_keys().m_account_address,false},{source.amount-fee-COIN,wallet.get_keys().m_account_address,false}};
  std::vector<cryptonote::tx_source_entry> sources{source};std::unordered_map<crypto::public_key,cryptonote::subaddress_index> subaddresses;subaddresses[wallet.get_keys().m_account_address.m_spend_public_key]={0,0};
  xgtx_exchange::operation op;op.chain=chain;op.deadline=deadline;op.quantity=1000000;op.ticker=argv[5];
  if(std::string(argv[1])=="build-operation"){
    std::ifstream file(argv[5],std::ios::binary);file.seekg(0,std::ios::end);auto size=static_cast<std::streamoff>(file.tellg());require(size>=212 && size<=224,"operation fixture size");file.seekg(0);xgtx_exchange::bytes encoded(static_cast<std::size_t>(size));read(file,encoded.data(),encoded.size());op=xgtx_exchange::decode(encoded);
    xgtx_exchange::id owner{};std::memcpy(owner.data(),&wallet.get_keys().m_account_address.m_spend_public_key,32);
    require(op.chain==chain && op.sender==owner && op.deadline<=deadline,"operation fixture chain/owner/expiry");
  }
  auto intent=xgtx_exchange::cryptonote_sign(op,wallet.get_keys().m_account_address.m_spend_public_key,wallet.get_keys().m_spend_secret_key);
  cryptonote::transaction tx;crypto::secret_key txkey{};std::vector<crypto::secret_key> additional;
  require(cryptonote::construct_exchange_tx_and_get_tx_key(wallet.get_keys(),subaddresses,sources,destinations,wallet.get_keys().m_account_address,{},tx,txkey,additional,true,{rct::RangeProofPaddedBulletproof,4},true,intent),"funded carrier native construction failed");
  xgtx_exchange::verify_native_carrier(tx,chain);rct::ctkeyM ring(1);for(auto& entry:source.outputs)ring[0].push_back(entry.second);
  require(cryptonote::ver_input_proofs_rings(tx,ring) && rct::verRctSemanticsSimple(tx.rct_signatures),"funded fixture native proof check");
  auto blob=cryptonote::tx_to_blob(tx);auto hash=cryptonote::get_transaction_hash(tx);auto image=boost::get<cryptonote::txin_to_key>(tx.vin[0]).k_image;
  std::cout<<"{\"tx_as_hex\":\""<<hex(blob.data(),blob.size())<<"\",\"hash\":\""<<hex(&hash,32)<<"\",\"key_image\":\""<<hex(&image,32)<<"\",\"fee\":"<<fee<<",\"weight\":"<<cryptonote::get_transaction_weight(tx)<<"}\n";
  return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
