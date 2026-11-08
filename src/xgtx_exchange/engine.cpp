#include "engine.hpp"
#include <algorithm>
#include <limits>
#include <utility>

namespace xgtx_exchange {
namespace {
constexpr amount max_amount=std::numeric_limits<amount>::max();
void require(bool good,const char* reason){if(!good)throw std::invalid_argument(reason);}
amount checked(wide n){require(n<=max_amount,"amount overflow");return static_cast<amount>(n);}
amount add(amount a,amount b){return checked(wide(a)+b);}
amount get(const std::map<std::pair<id,id>,amount>& m,const id& owner,const id& asset){auto i=m.find({owner,asset});return i==m.end()?0:i->second;}
void credit(std::map<std::pair<id,id>,amount>& m,const id& owner,const id& asset,amount n){if(n)m[{owner,asset}]=add(get(m,owner,asset),n);}
void debit(std::map<std::pair<id,id>,amount>& m,const id& owner,const id& asset,amount n){auto old=get(m,owner,asset);require(old>=n,"insufficient balance");if(old==n)m.erase({owner,asset});else m[{owner,asset}]=old-n;}
amount root(wide n){amount lo=0,hi=max_amount;while(lo<hi){auto mid=lo+(hi-lo)/2+1;if(wide(mid)*mid<=n)lo=mid;else hi=mid-1;}return lo;}
amount ceiling(wide n,amount d){require(d!=0,"zero divisor");return checked(n/d+(n%d!=0));}
struct writer {
  bytes b;
  void u8(std::uint8_t x){b.push_back(x);}
  void u64(amount x){for(unsigned i=0;i<8;++i)u8(static_cast<std::uint8_t>(x>>(8*i)));}
  void key(const id& x){b.insert(b.end(),x.begin(),x.end());}
  void text(const std::string& s){require(s.size()<=12,"string length");u8(static_cast<std::uint8_t>(s.size()));b.insert(b.end(),s.begin(),s.end());}
};
struct reader {
  const bytes& b;std::size_t p=0;
  void need(std::size_t n){require(p<=b.size() && n<=b.size()-p,"truncated encoding");}
  std::uint8_t u8(){need(1);return b[p++];}
  amount u64(){amount x=0;for(unsigned i=0;i<8;++i)x|=amount(u8())<<(8*i);return x;}
  id key(){need(32);id x{};std::copy_n(b.begin()+p,32,x.begin());p+=32;return x;}
  std::string text(){auto n=u8();require(n<=12,"string length");need(n);std::string x(b.begin()+p,b.begin()+p+n);p+=n;return x;}
  amount count(){auto n=u64();require(n<=1000000 && n<=b.size()-p,"state count");return n;}
  void end(){require(p==b.size(),"trailing bytes");}
};
void ticker_valid(const std::string& s){require(s.size()>=2 && s.size()<=12,"ticker length");for(std::size_t i=0;i<s.size();++i)require((s[i]>='A' && s[i]<='Z')||(i && s[i]>='0' && s[i]<='9'),"ticker character");}
void structural(const operation& o){
  auto k=static_cast<unsigned>(o.type);require(k>=1 && k<=9,"operation kind");
  require(o.sender!=id{} && o.chain!=id{} && o.nonce!=0,"chain/sender/nonce");
  require(o.quantity!=0,"zero quantity");
  if(o.type==kind::native_deposit || o.type==kind::native_withdraw){
    require(o.asset==native_asset && o.other==id{} && o.ticker.empty() && !o.decimals && !o.minimum,"native operation fields");
    if(o.type==kind::native_deposit)require(o.recipient==id{} && (o.limit==1 || o.limit==16),"native deposit output count");
    else require(o.recipient!=id{},"missing native payout binding");
    return;
  }
  if(o.type==kind::create){ticker_valid(o.ticker);require(o.ticker!="XGTX","reserved native ticker");require(o.decimals<=12,"decimals");require(o.asset==id{} && o.other==id{} && o.recipient==id{} && !o.limit && !o.minimum,"unused create fields");}
  else {require(o.ticker.empty() && !o.decimals,"unused metadata");
    if(o.type==kind::transfer)require(o.recipient!=id{} && o.other==id{} && !o.limit && !o.minimum,"transfer fields");
    else if(o.type==kind::burn)require(o.recipient==id{} && o.other==id{} && !o.limit && !o.minimum,"burn fields");
    else {require(o.recipient==id{} && o.asset!=o.other,"pool fields");
      if(o.type==kind::pool_create)require(o.limit && !o.minimum,"initial liquidity");
      if(o.type==kind::liquidity_add)require(o.limit && o.minimum,"deposit limits");
      if(o.type==kind::liquidity_remove)require(o.limit && o.minimum,"withdrawal limits");
      if(o.type==kind::swap)require(!o.limit && o.minimum,"swap limits");
    }
  }
}
}
bytes encode(const operation& o){
  structural(o);writer w;w.b={'X','G','T','X','D','E','X',0,1};w.u8(static_cast<std::uint8_t>(o.type));
  w.key(o.chain);w.key(o.sender);w.u64(o.nonce);w.u64(o.deadline);w.key(o.asset);w.key(o.other);w.key(o.recipient);
  w.u64(o.quantity);w.u64(o.limit);w.u64(o.minimum);w.u8(o.decimals);w.text(o.ticker);return w.b;
}
operation decode(const bytes& b){
  require(b.size()>=212 && b.size()<=224,"operation size");reader r{b};
  const bytes prefix={'X','G','T','X','D','E','X',0,1};for(auto x:prefix)require(r.u8()==x,"operation domain/version");
  operation o;o.type=static_cast<kind>(r.u8());o.chain=r.key();o.sender=r.key();o.nonce=r.u64();o.deadline=r.u64();o.asset=r.key();o.other=r.key();o.recipient=r.key();o.quantity=r.u64();o.limit=r.u64();o.minimum=r.u64();o.decimals=r.u8();o.ticker=r.text();r.end();structural(o);return o;
}
bytes signing_message(const operation& o){bytes b={'X','G','T','X','-','D','E','X','-','S','I','G','N',0,1};auto p=encode(o);b.insert(b.end(),p.begin(),p.end());return b;}
bytes encode_envelope(const envelope& e){auto b=encode(e.op);b.insert(b.end(),e.sig.begin(),e.sig.end());return b;}
envelope decode_envelope(const bytes& b){require(b.size()>=276 && b.size()<=288,"signed envelope size");envelope e;e.op=decode(bytes(b.begin(),b.end()-64));std::copy_n(b.end()-64,64,e.sig.begin());return e;}
engine::engine(config c,hasher hash,verifier verify):cfg_(c),hash_(std::move(hash)),verify_(std::move(verify)){
  require(cfg_.chain!=id{} && cfg_.treasury!=id{},"missing chain/treasury");require(cfg_.max_operations && cfg_.max_operations<=1024,"operation cap");require(bool(hash_) && bool(verify_),"missing crypto");
}
id engine::asset_id(const operation& o)const{require(o.type==kind::create,"not creation");auto b=encode(o);b.insert(b.begin(),{'A','S','S','E','T',0});auto a=hash_(b);require(a!=native_asset,"asset collision");return a;}
id engine::pool_id(id a,id b)const{require(a!=b,"same pool asset");if(b<a)std::swap(a,b);writer w;w.b={'X','G','T','X','-','P','O','O','L',0,1};w.key(cfg_.chain);w.key(a);w.key(b);return hash_(w.b);}
receipt engine::quote_swap(const state& s,id asset_in,id asset_out,amount quantity)const{
  require(quantity && asset_in!=asset_out,"invalid quote amount/pair");
  require((asset_in!=native_asset || cfg_.native_backing) && (asset_out!=native_asset || cfg_.native_backing) && s.assets.count(asset_in) && s.assets.count(asset_out),"unknown or unbridged quote asset");
  receipt r;r.pool_id=pool_id(asset_in,asset_out);auto i=s.pools.find(r.pool_id);require(i!=s.pools.end(),"unknown pool");
  const auto& p=i->second;require(p.shares && p.reserve_a && p.reserve_b,"empty pool");
  auto rin=asset_in<asset_out?p.reserve_a:p.reserve_b,rout=asset_in<asset_out?p.reserve_b:p.reserve_a;
  r.lp_fee=checked(wide(quantity)*25/10000);r.protocol_fee=checked(wide(quantity)*5/10000);auto effective=quantity-r.lp_fee-r.protocol_fee;
  r.output=checked(wide(rout)*effective/(wide(rin)+effective));require(r.output && r.output<rout,"swap slippage/dust");
  auto newin=add(rin,quantity-r.protocol_fee),newout=rout-r.output;require(wide(newin)*newout>=wide(rin)*rout,"pool invariant");return r;
}
receipt engine::apply(state& original,const envelope& e,amount height)const{
  return apply_internal(original,e,height,nullptr);
}
receipt engine::apply_verified_native(state& original,const envelope& e,amount height,const native_effect& effect)const{
  return apply_internal(original,e,height,&effect);
}
receipt engine::apply_internal(state& original,const envelope& e,amount height,const native_effect* effect)const{
  invariants(original);
  structural(e.op);require(height>=cfg_.activation_height,"not activated");require(e.op.chain==cfg_.chain,"wrong chain");require(e.op.deadline>=height,"expired");
  auto it=original.nonces.find(e.op.sender);auto n=it==original.nonces.end()?0:it->second;require(n<max_amount && e.op.nonce==n+1,"nonce/replay");
  require(verify_(e.op.sender,signing_message(e.op),e.sig),"invalid signature");
  const bool deposit=e.op.type==kind::native_deposit,withdraw=e.op.type==kind::native_withdraw;
  require((deposit || withdraw)==bool(effect),"native operation requires explicit verified effect");
  state candidate=original;receipt r;
  if(effect){
    require(cfg_.native_backing && effect->transaction!=id{},"native backing disabled or missing native transaction");
    require(effect->intent==hash_(encode_envelope(e)),"native effect intent binding mismatch");
    if(deposit){
      require(!effect->released && effect->locked==e.op.quantity && effect->payout==id{},"native deposit effect mismatch");
      candidate.native_locked=add(candidate.native_locked,e.op.quantity);
      if(!candidate.assets.count(native_asset))candidate.assets[native_asset]={cfg_.chain,0,10,"XGTX"};
      candidate.assets.at(native_asset).supply=candidate.native_locked;
      credit(candidate.balances,e.op.sender,native_asset,e.op.quantity);
    }else{
      require(effect->released>=effect->locked && effect->released-effect->locked==e.op.quantity && effect->locked==e.op.limit,"native withdrawal effect mismatch");
      require(effect->payout==e.op.recipient,"native payout binding mismatch");
      require(candidate.native_locked>=e.op.quantity && candidate.assets.count(native_asset),"native reserve underflow");
      debit(candidate.balances,e.op.sender,native_asset,e.op.quantity);
      candidate.native_locked-=e.op.quantity;candidate.assets.at(native_asset).supply=candidate.native_locked;
    }
    r.asset=native_asset;r.output=e.op.quantity;
  }else r=transition(candidate,e.op);
  candidate.nonces[e.op.sender]=e.op.nonce;invariants(candidate);serialize(candidate);original=std::move(candidate);return r;
}
block_result engine::apply_block(const state& original,amount height,const id& parent,const id& block,const std::vector<envelope>& ops)const{
  require(ops.size()<=cfg_.max_operations,"block operation cap");
  std::vector<native_block_operation> verified;verified.reserve(ops.size());
  for(const auto& op:ops)verified.push_back({op,std::nullopt});
  return apply_verified_block(original,height,parent,block,verified);
}
block_result engine::apply_verified_block(const state& original,amount height,const id& parent,const id& block,const std::vector<native_block_operation>& ops)const{
  require(original.height<max_amount && height==original.height+1,"block height");require(parent==original.tip && block!=id{} && block!=parent,"block linkage");
  require(ops.size()<=cfg_.max_operations,"block operation cap");
  block_result r{original,{}};std::set<id> native_transactions;
  for(const auto& op:ops){
    if(op.effect)require(native_transactions.insert(op.effect->transaction).second,"duplicate native effect transaction");
    r.receipts.push_back(op.effect ? apply_verified_native(r.next,op.intent,height,*op.effect) : apply(r.next,op.intent,height));
  }
  r.next.height=height;r.next.tip=block;invariants(r.next);return r;
}
receipt engine::transition(state& s,const operation& o)const{
  receipt r;
  if(o.type==kind::create){auto a=asset_id(o);require(!s.assets.count(a),"asset already exists");s.assets[a]={o.sender,o.quantity,o.decimals,o.ticker};credit(s.balances,o.sender,a,o.quantity);r.asset=a;return r;}
  // Native XGTX is deliberately unavailable until an authenticated lock/release
  // bridge is connected. A public balance must never be minted from an RPC claim.
  require((o.asset!=native_asset || cfg_.native_backing) && s.assets.count(o.asset),"unknown or unbridged asset");
  if(o.type==kind::transfer){debit(s.balances,o.sender,o.asset,o.quantity);credit(s.balances,o.recipient,o.asset,o.quantity);return r;}
  if(o.type==kind::burn){require(o.asset!=native_asset,"native XGTX requires authenticated withdrawal");debit(s.balances,o.sender,o.asset,o.quantity);auto& a=s.assets.at(o.asset);require(a.supply>=o.quantity,"supply underflow");a.supply-=o.quantity;return r;}
  require((o.other!=native_asset || cfg_.native_backing) && s.assets.count(o.other),"unknown or unbridged pool asset");
  id a=o.asset,b=o.other;bool forward=a<b;if(!forward)std::swap(a,b);auto pid=pool_id(a,b);r.pool_id=pid;
  if(o.type==kind::pool_create){require(!s.pools.count(pid),"pool exists");auto shares=root(wide(o.quantity)*o.limit);require(shares,"zero LP shares");debit(s.balances,o.sender,o.asset,o.quantity);debit(s.balances,o.sender,o.other,o.limit);s.pools[pid]={a,b,forward?o.quantity:o.limit,forward?o.limit:o.quantity,shares};credit(s.lp,o.sender,pid,shares);r.shares=shares;r.used_a=o.quantity;r.used_b=o.limit;return r;}
  auto i=s.pools.find(pid);require(i!=s.pools.end(),"unknown pool");auto& p=i->second;require(p.shares && p.reserve_a && p.reserve_b,"empty pool");
  auto rin=forward?p.reserve_a:p.reserve_b,rout=forward?p.reserve_b:p.reserve_a;
  if(o.type==kind::liquidity_add){auto shares=std::min(checked(wide(o.quantity)*p.shares/rin),checked(wide(o.limit)*p.shares/rout));require(shares && shares>=o.minimum,"LP slippage");auto da=ceiling(wide(shares)*rin,p.shares),db=ceiling(wide(shares)*rout,p.shares);require(da<=o.quantity && db<=o.limit,"deposit limits");debit(s.balances,o.sender,o.asset,da);debit(s.balances,o.sender,o.other,db);if(forward){p.reserve_a=add(p.reserve_a,da);p.reserve_b=add(p.reserve_b,db);}else{p.reserve_b=add(p.reserve_b,da);p.reserve_a=add(p.reserve_a,db);}p.shares=add(p.shares,shares);credit(s.lp,o.sender,pid,shares);r.shares=shares;r.used_a=da;r.used_b=db;return r;}
  if(o.type==kind::liquidity_remove){require(o.quantity<=p.shares,"LP supply");debit(s.lp,o.sender,pid,o.quantity);auto da=checked(wide(o.quantity)*rin/p.shares),db=checked(wide(o.quantity)*rout/p.shares);require(da>=o.minimum && db>=o.limit,"withdrawal slippage");credit(s.balances,o.sender,o.asset,da);credit(s.balances,o.sender,o.other,db);if(forward){p.reserve_a-=da;p.reserve_b-=db;}else{p.reserve_b-=da;p.reserve_a-=db;}p.shares-=o.quantity;r.used_a=da;r.used_b=db;r.shares=o.quantity;if(!p.shares)s.pools.erase(i);return r;}
  require(o.type==kind::swap,"operation kind");
  r=quote_swap(s,o.asset,o.other,o.quantity);require(r.output>=o.minimum,"swap slippage/dust");
  auto newin=add(rin,o.quantity-r.protocol_fee),newout=rout-r.output;
  debit(s.balances,o.sender,o.asset,o.quantity);credit(s.balances,o.sender,o.other,r.output);credit(s.balances,cfg_.treasury,o.asset,r.protocol_fee);
  if(forward){p.reserve_a=newin;p.reserve_b=newout;}else{p.reserve_b=newin;p.reserve_a=newout;}return r;
}
void engine::invariants(const state& s)const{
  require((s.height==0)==(s.tip==id{}),"state height/tip mismatch");
  std::map<id,wide> totals,shares;
  for(auto& [k,n]:s.balances){require(n && k.first!=id{} && s.assets.count(k.second),"invalid balance");totals[k.second]+=n;require(totals[k.second]<=max_amount,"aggregate overflow");}
  for(auto& [k,n]:s.lp){require(n && k.first!=id{} && s.pools.count(k.second),"invalid LP balance");shares[k.second]+=n;require(shares[k.second]<=max_amount,"LP aggregate overflow");}
  for(auto& [pid,p]:s.pools){require(p.a<p.b && s.assets.count(p.a) && s.assets.count(p.b) && pid==pool_id(p.a,p.b),"pool identity");require(p.reserve_a && p.reserve_b && p.shares && shares[pid]==p.shares,"pool/LP conservation");totals[p.a]+=p.reserve_a;totals[p.b]+=p.reserve_b;require(totals[p.a]<=max_amount && totals[p.b]<=max_amount,"reserve aggregate overflow");}
  require(!s.native_locked || s.assets.count(native_asset),"native backing has no ledger asset");
  for(auto& [a,info]:s.assets){
    if(a==native_asset)require(cfg_.native_backing && info.issuer==cfg_.chain && info.decimals==10 && info.ticker=="XGTX" && info.supply==s.native_locked,"native backing metadata/supply mismatch");
    else {require(info.issuer!=id{} && info.decimals<=12 && s.nonces.count(info.issuer),"asset metadata/issuance nonce");ticker_valid(info.ticker);}
    require(totals[a]==info.supply,"asset conservation");
  }
  for(auto& [owner,n]:s.nonces)require(owner!=id{} && n,"invalid nonce state");
}
bytes serialize(const state& s){
  require(s.assets.size()<=1000000 && s.balances.size()<=1000000 && s.lp.size()<=1000000 && s.nonces.size()<=1000000 && s.pools.size()<=1000000,"state entry cap");
  writer w;w.b={'X','G','T','X','S','T',0,1};const bool backed=s.native_locked || s.assets.count(native_asset);if(backed)w.b.back()=2;w.u64(s.height);w.key(s.tip);if(backed)w.u64(s.native_locked);
  w.u64(s.assets.size());for(auto& [a,v]:s.assets){w.key(a);w.key(v.issuer);w.u64(v.supply);w.u8(v.decimals);w.text(v.ticker);}
  auto pairs=[&](const auto& m){w.u64(m.size());for(auto& [k,n]:m){w.key(k.first);w.key(k.second);w.u64(n);}};
  pairs(s.balances);pairs(s.lp);w.u64(s.nonces.size());for(auto& [k,n]:s.nonces){w.key(k);w.u64(n);}
  w.u64(s.pools.size());for(auto& [k,p]:s.pools){w.key(k);w.key(p.a);w.key(p.b);w.u64(p.reserve_a);w.u64(p.reserve_b);w.u64(p.shares);}require(w.b.size()<=64*1024*1024,"state size cap");return w.b;
}
state deserialize(const bytes& b){
  require(b.size()<=64*1024*1024,"state size cap");reader r{b};for(auto x:bytes{'X','G','T','X','S','T',0})require(r.u8()==x,"state domain");const auto version=r.u8();require(version==1 || version==2,"state version");state s;s.height=r.u64();s.tip=r.key();if(version==2)s.native_locked=r.u64();
  auto count=r.count();for(amount j=0;j<count;++j){auto a=r.key();asset_info v;v.issuer=r.key();v.supply=r.u64();v.decimals=r.u8();v.ticker=r.text();require(s.assets.emplace(a,v).second,"duplicate asset");}
  auto pairs=[&](auto& m){auto n=r.count();for(amount j=0;j<n;++j){auto a=r.key(),bkey=r.key();auto v=r.u64();require(m.emplace(std::make_pair(a,bkey),v).second,"duplicate balance");}};
  pairs(s.balances);pairs(s.lp);count=r.count();for(amount j=0;j<count;++j){auto k=r.key();auto n=r.u64();require(s.nonces.emplace(k,n).second,"duplicate nonce");}
  count=r.count();for(amount j=0;j<count;++j){auto k=r.key();pool p;p.a=r.key();p.b=r.key();p.reserve_a=r.u64();p.reserve_b=r.u64();p.shares=r.u64();require(s.pools.emplace(k,p).second,"duplicate pool");}r.end();require(serialize(s)==b,"noncanonical state order");return s;
}
bool state::operator==(const state& rhs)const{return serialize(*this)==serialize(rhs);}
}
