#pragma once
#include "engine.hpp"
#include <algorithm>

namespace xgtx_exchange {
namespace undo_detail {
inline const bytes magic={'X','G','T','X','U','D',0,1};
inline constexpr std::size_t max_bytes=256*1024*1024;
struct writer {
  bytes data;
  void byte(std::uint8_t n){if(data.size()==max_bytes)throw std::invalid_argument("undo too large");data.push_back(n);}
  void value(amount n){for(unsigned i=0;i<8;++i)byte(static_cast<std::uint8_t>(n>>(i*8)));}
  void value(const id& k){for(auto c:k)byte(c);}
  void value(const std::pair<id,id>& k){value(k.first);value(k.second);}
  void value(const asset_info& a){value(a.issuer);value(a.supply);byte(a.decimals);if(a.ticker.size()>12)throw std::invalid_argument("undo ticker");byte(static_cast<std::uint8_t>(a.ticker.size()));for(auto c:a.ticker)byte(c);}
  void value(const pool& p){value(p.a);value(p.b);value(p.reserve_a);value(p.reserve_b);value(p.shares);}
};
struct reader {
  const bytes& data;std::size_t pos=0;
  std::uint8_t byte(){if(pos==data.size())throw std::invalid_argument("truncated undo");return data[pos++];}
  void value(amount& n){n=0;for(unsigned i=0;i<8;++i)n|=amount(byte())<<(8*i);}
  void value(id& k){for(auto& c:k)c=byte();}
  void value(std::pair<id,id>& k){value(k.first);value(k.second);}
  void value(asset_info& a){value(a.issuer);value(a.supply);a.decimals=byte();auto n=byte();if(n>12)throw std::invalid_argument("undo ticker");a.ticker.clear();for(unsigned i=0;i<n;++i)a.ticker.push_back(static_cast<char>(byte()));}
  void value(pool& p){value(p.a);value(p.b);value(p.reserve_a);value(p.reserve_b);value(p.shares);}
};
template<class V> bool same(const V& a,const V& b){writer x,y;x.value(a);y.value(b);return x.data==y.data;}
// Merge ordered maps so each changed key occurs once. Store its old value or
// an absent marker; unchanged entries take no journal space.
template<class M> void diff(writer& out,const M& before,const M& after){
  writer changes;amount count=0;auto a=before.begin(),b=after.begin();
  while(a!=before.end() || b!=after.end()){
    if(b==after.end() || (a!=before.end() && a->first<b->first)){
      changes.value(a->first);changes.byte(1);changes.value(a->second);++a;++count;
    }else if(a==before.end() || b->first<a->first){
      changes.value(b->first);changes.byte(0);++b;++count;
    }else{if(!same(a->second,b->second)){changes.value(a->first);changes.byte(1);changes.value(a->second);++count;}++a;++b;}
  }
  if(count>2000000)throw std::invalid_argument("undo entry cap");
  out.value(count);for(auto c:changes.data)out.byte(c);
}
template<class M> void restore(reader& in,M& values){
  amount count;in.value(count);if(count>2000000 || count>in.data.size()-in.pos)throw std::invalid_argument("undo entry cap");
  typename M::key_type previous{};bool have_previous=false;
  for(amount i=0;i<count;++i){
    typename M::key_type key;in.value(key);
    if(have_previous && !(previous<key))throw std::invalid_argument("unordered undo keys");
    previous=key;have_previous=true;auto present=in.byte();auto found=values.find(key);
    if(present==0){if(found==values.end())throw std::invalid_argument("redundant undo deletion");values.erase(found);}
    else if(present==1){typename M::mapped_type old;in.value(old);if(found!=values.end() && same(found->second,old))throw std::invalid_argument("redundant undo value");values[key]=std::move(old);}
    else throw std::invalid_argument("undo presence marker");
  }
}
}
inline bool is_state_undo(const bytes& data){return data.size()>=undo_detail::magic.size() && std::equal(undo_detail::magic.begin(),undo_detail::magic.end(),data.begin());}
inline bytes encode_state_undo(const state& before,const state& after,const engine& e){
  if(after.height==0 || before.height!=after.height-1)throw std::invalid_argument("undo height");
  undo_detail::writer out;out.data=undo_detail::magic;
  out.value(e.state_commitment(before));out.value(e.state_commitment(after));
  out.value(before.height);out.value(before.tip);out.value(before.native_locked);
  undo_detail::diff(out,before.assets,after.assets);undo_detail::diff(out,before.balances,after.balances);
  undo_detail::diff(out,before.lp,after.lp);undo_detail::diff(out,before.nonces,after.nonces);undo_detail::diff(out,before.pools,after.pools);
  return out.data;
}
inline state restore_state_undo(const state& current,const bytes& data,const engine& e){
  if(data.size()>undo_detail::max_bytes || !is_state_undo(data))throw std::invalid_argument("undo format");
  undo_detail::reader in{data,undo_detail::magic.size()};id before_hash,after_hash;in.value(before_hash);in.value(after_hash);
  if(e.state_commitment(current)!=after_hash)throw std::invalid_argument("undo current state mismatch");
  auto prior=current;in.value(prior.height);in.value(prior.tip);in.value(prior.native_locked);
  if(!current.height || prior.height!=current.height-1)throw std::invalid_argument("undo height");
  undo_detail::restore(in,prior.assets);undo_detail::restore(in,prior.balances);undo_detail::restore(in,prior.lp);undo_detail::restore(in,prior.nonces);undo_detail::restore(in,prior.pools);
  if(in.pos!=data.size() || e.state_commitment(prior)!=before_hash)throw std::invalid_argument("undo prior state mismatch");
  return prior;
}
}
