#include "../../src/xgtx_exchange/native_chain.hpp"
#include "../../src/xgtx_exchange/fee_recipient.hpp"
#include "blockchain_db/lmdb/db_lmdb.h"
#include "cryptonote_core/cryptonote_tx_utils.h"
#include "cryptonote_core/tx_verification_utils.h"
#include "cryptonote_basic/account.h"
#include "cryptonote_config.h"
#include "string_tools.h"
#include <array>
#include <algorithm>
#include <limits>
#include <cstring>
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
  std::cout<<"{\"native_locked\":"<<state.native_locked<<",\"height\":"<<state.height<<",\"tip\":\""<<idhex(state.tip)<<"\",\"assets\":[";
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
namespace {
using namespace xgtx_exchange;
id parse_id(const std::string& text){id out{};require(text.size()==64,"chain hash size");require(epee::string_tools::hex_to_pod(text,out),"chain hash encoding");return out;}
void emit(const cryptonote::transaction& tx,amount debit=0){
  auto blob=cryptonote::tx_to_blob(tx);auto hash=cryptonote::get_transaction_hash(tx);
  require(cryptonote::check_money_overflow(tx),"native money overflow check");
  require(cryptonote::check_output_types(tx,16),"native tagged output rules");
  std::cout<<"{\"tx_as_hex\":\""<<hex(blob.data(),blob.size())<<"\",\"hash\":\""<<hex(&hash,32)<<"\",\"debit\":"<<debit<<",\"fee\":"<<tx.rct_signatures.txnFee<<"}\n";
}
void output(cryptonote::transaction& tx,const crypto::public_key& key,const crypto::view_tag& tag={}){
  cryptonote::tx_out out;out.amount=0;out.target=cryptonote::txout_to_tagged_key(key,tag);tx.vout.push_back(out);
}
void restore_parameters(const char* keys,const char* path){
  cryptonote::account_base wallet;load_wallet(keys,wallet);const auto& k=wallet.get_keys();
  std::string json="{\"restore_height\":0,\"filename\":\"payee\",\"address\":\""+cryptonote::get_account_address_as_str(cryptonote::MAINNET,false,k.m_account_address)+"\",\"spendkey\":\""+hex(&k.m_spend_secret_key,32)+"\",\"viewkey\":\""+hex(&k.m_view_secret_key,32)+"\",\"password\":\"\",\"language\":\"English\"}";
  int fd=::open(path,O_WRONLY|O_CREAT|O_EXCL,0600);require(fd>=0,"cannot create private fixture restore parameters");
  std::size_t done=0;while(done<json.size()){auto n=::write(fd,json.data()+done,json.size()-done);if(n<=0){::close(fd);throw std::runtime_error("restore fixture write failed");}done+=n;}require(::close(fd)==0,"restore fixture close failed");std::cout<<"{\"private_file_created\":true}\n";
}
void build_deposit(const char* keys,const char* path,unsigned position){
  cryptonote::account_base wallet;load_wallet(keys,wallet);const auto& k=wallet.get_keys();
  std::ifstream input(path,std::ios::binary);char magic[9];read(input,magic,9);require(std::memcmp(magic,"XGTXFUND1",9)==0,"funding fixture domain");
  id chain{};read(input,chain.data(),32);auto deadline=number(input,8),fee=number(input,8),count=number(input,4);
  require(fee==COIN && count==16 && position<count,"deposit funding parameters");
  std::vector<cryptonote::block> blocks;std::vector<std::uint64_t> offsets;
  for(unsigned i=0;i<count;++i){offsets.push_back(number(input,8));auto size=number(input,4);require(size>0 && size<1024*1024,"funding block size");cryptonote::blobdata blob(size,'\0');read(input,blob.data(),size);cryptonote::block b;require(cryptonote::parse_and_validate_block_from_blob(cryptonote::blobdata_ref{blob.data(),blob.size()},b),"funding block parse");require(b.miner_tx.version==2 && b.miner_tx.vout.size()==1,"single modern coinbase required");blocks.push_back(b);}
  require(input.peek()==std::char_traits<char>::eof(),"funding trailing bytes");
  amount total=blocks[position].miner_tx.vout[0].amount;require(total>fee+16,"deposit amount");
  crypto::key_derivation derivation{};require(crypto::generate_key_derivation(cryptonote::get_tx_pub_key_from_extra(blocks[position].miner_tx),k.m_view_secret_key,derivation),"coinbase derivation");
  crypto::secret_key ephemeral{};crypto::derive_secret_key(derivation,0,k.m_spend_secret_key,ephemeral);crypto::public_key actual{},expected{};
  require(crypto::secret_key_to_public_key(ephemeral,actual) && cryptonote::get_output_public_key(blocks[position].miner_tx.vout[0],expected) && actual==expected,"coinbase belongs to test wallet");
  operation op;op.type=kind::native_deposit;op.chain=chain;op.deadline=deadline;op.quantity=total-fee;op.limit=16;
  auto intent=cryptonote_sign(op,k.m_account_address.m_spend_public_key,k.m_spend_secret_key);
  cryptonote::transaction original;original.version=2;cryptonote::txin_to_key in{};in.amount=0;crypto::generate_key_image(actual,ephemeral,in.k_image);
  for(unsigned i=0;i<count;++i){require(!i || offsets[i]>offsets[i-1],"native ring offsets increasing");in.key_offsets.push_back(i?offsets[i]-offsets[i-1]:offsets[i]);}original.vin.push_back(in);
  rct::ctkeyM mix(1);for(const auto& b:blocks){crypto::public_key pub{};require(cryptonote::get_output_public_key(b.miner_tx.vout[0],pub),"coinbase output key");mix[0].push_back({rct::pk2rct(pub),rct::zeroCommitVartime(b.miner_tx.vout[0].amount)});}
  rct::keyV destinations,amount_keys;std::vector<rct::xmr_amount> amounts;
  for(unsigned i=0;i<16;++i){auto key=reserve_proofs::derive({chain,intent.op.sender,intent.op.nonce},i);output(original,key.public_key);destinations.push_back(rct::pk2rct(key.public_key));amount_keys.push_back(key.amount_key);amounts.push_back(deposit_part(op.quantity,16,i));}
  // Reserve destinations use independent public domains; the tx public key is
  // not a recipient derivation. No ordinary wallet payout is present here.
  cryptonote::add_tx_pub_key_to_extra(original,k.m_account_address.m_spend_public_key);
  rct::ctkeyV secrets{{rct::sk2rct(ephemeral),rct::identity()}},out_secrets;std::vector<rct::xmr_amount> inputs{total};std::vector<unsigned> real{position};
  auto prefix=cryptonote::get_transaction_prefix_hash(original);auto& device=hw::get_device("default");
  auto tx=sign_carrier_transaction(original,intent,k.m_account_address.m_spend_public_key,k.m_spend_secret_key,
    [&](const std::function<rct::key(const rct::rctSig&)>& finalize){return rct::genRctSimpleWithMessageFinalizer(rct::hash2rct(prefix),secrets,destinations,inputs,amounts,fee,mix,amount_keys,real,out_secrets,{rct::RangeProofPaddedBulletproof,4},device,finalize);});
  require(cryptonote::ver_input_proofs_rings(tx,mix) && rct::verRctSemanticsSimple(tx.rct_signatures),"native deposit proofs");emit(tx,op.quantity);
}
struct reserve_input {cryptonote::txin_to_key input;rct::ctkey secret{};rct::ctkeyV ring;amount value=0;unsigned real=0;};
void build_withdraw(const char* keys,const char* dbpath,const std::string& chain_hex,const char* recipient,amount nonce,amount debit){
  cryptonote::account_base owner,payee;load_wallet(keys,owner);load_wallet(recipient,payee);const auto& k=owner.get_keys();id owner_id{};std::memcpy(owner_id.data(),&k.m_account_address.m_spend_public_key,32);
  auto cfg=designated_fee_config(parse_id(chain_hex),2);cfg.native_backing=true;
  cryptonote::BlockchainLMDB db;db.configure_exchange_for_tests(cfg);db.open(dbpath,DBF_RDONLY);
  require(debit>COIN,"withdrawal includes native fee and positive payout");
  std::vector<reserve_input> inputs;amount total=0;const auto count=db.get_num_outputs(reserve_policy::bucket);require(count>=16,"reserve ring bootstrap missing");
  for(std::uint64_t index=0;index<count && total<debit;++index){
    auto location=db.get_output_tx_and_index(reserve_policy::bucket,index);auto origin=db.get_pruned_tx(location.first);auto intent=verify_native_carrier(origin,cfg.chain);
    amount value=intent.op.type==kind::native_deposit?deposit_part(intent.op.quantity,intent.op.limit,location.second):intent.op.limit;
    reserve_proofs::domain domain{cfg.chain,intent.op.sender,intent.op.nonce};auto opening=reserve_proofs::derive(domain,location.second);auto verified=reserve_proofs::inspect_output(origin,domain,location.second,value);
    if(db.has_key_image(verified.image))continue;
    require(inputs.size()<16 && value<=std::numeric_limits<amount>::max()-total,"fixture reserve selection cap");
    reserve_input source;source.input.amount=reserve_policy::bucket;source.input.k_image=verified.image;source.value=value;source.secret={opening.spend,verified.mask};
    const std::uint64_t start=index<16?0:index-15;source.real=index-start;source.input.key_offsets.assign(16,1);source.input.key_offsets[0]=start;
    for(unsigned i=0;i<16;++i){auto record=db.get_output_key(reserve_policy::bucket,start+i,true);source.ring.push_back({rct::pk2rct(record.pubkey),record.commitment});}
    total+=value;inputs.push_back(std::move(source));
  }
  require(total>=debit,"insufficient unspent reserve outputs");amount change=total-debit;
  std::sort(inputs.begin(),inputs.end(),[](const reserve_input& a,const reserve_input& b){return std::memcmp(&a.input.k_image,&b.input.k_image,32)>0;});
  operation op;op.type=kind::native_withdraw;op.chain=cfg.chain;op.nonce=nonce;op.deadline=db.height()+100;op.quantity=debit;op.limit=change;
  cryptonote::transaction original;original.version=2;rct::ctkeyV secrets,out_secrets;rct::ctkeyM mix;std::vector<unsigned> real;std::vector<rct::xmr_amount> inamounts,outamounts;rct::keyV destinations,amount_keys;
  for(const auto& source:inputs){original.vin.push_back(source.input);secrets.push_back(source.secret);mix.push_back(source.ring);real.push_back(source.real);inamounts.push_back(source.value);}
  crypto::secret_key txsecret{};crypto::public_key txpublic{};crypto::generate_keys(txpublic,txsecret);cryptonote::add_tx_pub_key_to_extra(original,txpublic);
  for(unsigned i=0;i<2;++i){
    if(change && i==0){auto key=reserve_proofs::derive({cfg.chain,owner_id,nonce},i);output(original,key.public_key);destinations.push_back(rct::pk2rct(key.public_key));amount_keys.push_back(key.amount_key);outamounts.push_back(change);}
    else {crypto::key_derivation derivation{};require(crypto::generate_key_derivation(payee.get_keys().m_account_address.m_view_public_key,txsecret,derivation),"payout key derivation");crypto::public_key key{};require(crypto::derive_public_key(derivation,i,payee.get_keys().m_account_address.m_spend_public_key,key),"payout one-time key");crypto::secret_key scalar{};crypto::derivation_to_scalar(derivation,i,scalar);crypto::view_tag tag{};crypto::derive_view_tag(derivation,i,tag);output(original,key,tag);destinations.push_back(rct::pk2rct(key));amount_keys.push_back(rct::sk2rct(scalar));auto payout=debit-COIN;outamounts.push_back(change?payout:(i==0?payout/2:payout-payout/2));}
  }
  op.recipient=withdrawal_payout_binding(original,cfg.chain,change!=0);auto intent=cryptonote_sign(op,k.m_account_address.m_spend_public_key,k.m_spend_secret_key);auto prefix=cryptonote::get_transaction_prefix_hash(original);auto& device=hw::get_device("default");
  auto tx=sign_carrier_transaction(original,intent,k.m_account_address.m_spend_public_key,k.m_spend_secret_key,
    [&](const std::function<rct::key(const rct::rctSig&)>& finalize){return rct::genRctSimpleWithMessageFinalizer(rct::hash2rct(prefix),secrets,destinations,inamounts,outamounts,COIN,mix,amount_keys,real,out_secrets,{rct::RangeProofPaddedBulletproof,4},device,finalize);});
  engine adapter(cfg,cryptonote_hash,cryptonote_verify);withdrawal_effect_from_database(tx,adapter,db);
  require(cryptonote::ver_input_proofs_rings(tx,mix) && rct::verRctSemanticsSimple(tx.rct_signatures),"native withdrawal proofs");emit(tx,debit);db.close();
}
}
int main(int argc,char** argv){try{
  require(argc>=3,"bridge builder command missing");std::string mode=argv[1];
  if(mode=="create"){require(argc==3,"create arguments");cryptonote::account_base wallet;wallet.generate();store_wallet(argv[2],wallet);std::cout<<"{\"address\":\""<<cryptonote::get_account_address_as_str(cryptonote::MAINNET,false,wallet.get_keys().m_account_address)<<"\",\"owner\":\""<<hex(&wallet.get_keys().m_account_address.m_spend_public_key,32)<<"\"}\n";}
  else if(mode=="inspect"){require(argc==3,"inspect arguments");inspect(argv[2]);}
  else if(mode=="restore-file"){require(argc==4,"restore arguments");restore_parameters(argv[2],argv[3]);}
  else if(mode=="deposit"){require(argc==5,"deposit arguments");build_deposit(argv[2],argv[3],std::stoul(argv[4]));}
  else if(mode=="withdraw"){require(argc==8,"withdraw arguments");build_withdraw(argv[2],argv[3],argv[4],argv[5],std::stoull(argv[6]),std::stoull(argv[7]));}
  else throw std::invalid_argument("unknown fixture command");return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
