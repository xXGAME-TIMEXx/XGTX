#pragma once
#include <array>
#include <cstdint>
#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

// Experimental wire format only. Not accepted by node consensus.
namespace xgtx_draft {
using bytes32 = std::array<std::uint8_t,32>;
struct operation {
  std::uint8_t kind=1; // 1=create, 2=transfer, 3=burn
  bytes32 chain{}, nonce{}, asset{}, recipient{};
  std::uint64_t amount=1;
  std::uint8_t decimals=0;
  std::string ticker;
};
inline void validate(const operation& o) {
  if(o.kind<1 || o.kind>3 || !o.amount) throw std::invalid_argument("kind/amount");
  if(o.kind==1) {
    if(o.decimals>12 || o.ticker.size()<2 || o.ticker.size()>12) throw std::invalid_argument("metadata");
    for(std::size_t i=0;i<o.ticker.size();++i) {
      const char c=o.ticker[i];
      if(!((c>='A' && c<='Z') || (i && c>='0' && c<='9'))) throw std::invalid_argument("ticker");
    }
    if(o.asset!=bytes32{} || o.recipient!=bytes32{}) throw std::invalid_argument("unused create fields");
  } else {
    if(o.decimals || !o.ticker.empty()) throw std::invalid_argument("unused metadata");
    if(o.kind==3 && o.recipient!=bytes32{}) throw std::invalid_argument("unused burn recipient");
  }
}
inline std::vector<std::uint8_t> encode(const operation& o) {
  validate(o);
  std::vector<std::uint8_t> b={'X','G','T','X','O','P',0,1,o.kind};
  auto put=[&](const bytes32& a){b.insert(b.end(),a.begin(),a.end());};
  put(o.chain); put(o.nonce);
  if(o.kind!=1) put(o.asset);
  for(unsigned i=0;i<8;++i) b.push_back(static_cast<std::uint8_t>(o.amount>>(8*i)));
  if(o.kind==1) {
    b.push_back(o.decimals); b.push_back(static_cast<std::uint8_t>(o.ticker.size()));
    b.insert(b.end(),o.ticker.begin(),o.ticker.end());
  } else if(o.kind==2) put(o.recipient);
  return b;
}
inline operation decode(const std::vector<std::uint8_t>& b) {
  if(b.size()<9 || b.size()>145) throw std::invalid_argument("size");
  const std::vector<std::uint8_t> prefix={'X','G','T','X','O','P',0,1};
  if(!std::equal(prefix.begin(),prefix.end(),b.begin())) throw std::invalid_argument("prefix/version");
  operation o; o.kind=b[8];
  if(o.kind<1 || o.kind>3) throw std::invalid_argument("kind");
  std::size_t p=9;
  auto need=[&](std::size_t n){if(n>b.size()-p) throw std::invalid_argument("truncated");};
  auto get=[&](bytes32& a){need(32);std::copy_n(b.begin()+p,32,a.begin());p+=32;};
  get(o.chain);get(o.nonce);
  if(o.kind!=1)get(o.asset);
  need(8);o.amount=0;
  for(unsigned i=0;i<8;++i)o.amount|=std::uint64_t(b[p++])<<(8*i);
  if(o.kind==1){need(2);o.decimals=b[p++];auto n=b[p++];if(n<2 || n>12)throw std::invalid_argument("ticker length");need(n);o.ticker.assign(b.begin()+p,b.begin()+p+n);p+=n;}
  else if(o.kind==2)get(o.recipient);
  if(p!=b.size())throw std::invalid_argument("trailing bytes");
  validate(o);return o;
}
}
