#include "../../src/xgtx_exchange/fee_recipient.hpp"
#include "../../src/xgtx_exchange/cryptonote_crypto.hpp"
#include <iostream>

namespace {
int checks=0;
void check(bool ok,const char* label){++checks;if(!ok)throw std::runtime_error(label);}
template<class F>void reject(F f,const char* label){bool rejected=false;try{f();}catch(const std::invalid_argument&){rejected=true;}check(rejected,label);}
std::string hex(const xgtx_exchange::id& a){const char* digits="0123456789abcdef";std::string out;for(auto n:a){out+=digits[n>>4];out+=digits[n&15];}return out;}
}
int main(){try{
  using namespace xgtx_exchange;
  auto recipient=parse_fee_recipient(cryptonote::MAINNET,designated_fee_address);
  check(recipient.address.size()==95,"standard address size");
  check(recipient.address==designated_fee_address,"configured address preserved");
  check(hex(recipient.spend)=="583e13bbd119abe8c7c773c5f7d6602244ca331eca637441a252fbc130fea58a","spend key mapping");
  check(hex(recipient.view)=="2ed3763141325dfc1865013f7042228ef81dc7c73b1ec6965a896c7f68f30039","view key mapping");
  id chain{};chain.fill(0x91);auto cfg=designated_fee_config(chain,1);
  check(cfg.treasury==recipient.spend && cfg.treasury!=recipient.view,"treasury authorizes spend key");
  check(cfg.chain==chain && cfg.activation_height==1 && cfg.max_operations==1024,"explicit policy retained");
  engine configured(cfg,cryptonote_hash,cryptonote_verify);
  check(configured.settings().treasury==recipient.spend,"engine recipient binding");
  reject([&]{parse_fee_recipient(cryptonote::TESTNET,designated_fee_address);},"wrong testnet");
  reject([&]{parse_fee_recipient(cryptonote::STAGENET,designated_fee_address);},"wrong stagenet");
  std::string truncated=designated_fee_address;truncated.resize(89);
  reject([&]{parse_fee_recipient(cryptonote::MAINNET,truncated);},"truncated address");
  std::string corrupted=designated_fee_address;corrupted.back()=corrupted.back()=='1'?'2':'1';
  reject([&]{parse_fee_recipient(cryptonote::MAINNET,corrupted);},"checksum corruption");
  reject([&]{designated_fee_config(id{},1);},"missing chain");
  reject([&]{designated_fee_config(chain,0);},"missing activation");
  std::cout<<checks<<" checks passed: native address parsing, checksum, network, public key mapping, recipient policy\n";
  std::cout<<"Designated public address: "<<recipient.address<<"\n";
  std::cout<<"Configuration validation only; no fees or native exchange activation.\n";
  return 0;
}catch(const std::exception& ex){std::cerr<<"FAIL: "<<ex.what()<<"\n";return 1;}}
