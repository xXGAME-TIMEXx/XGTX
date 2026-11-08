#pragma once
#include "engine.hpp"
#include <limits>
#include <set>
#include <utility>

// Experimental accounting only. NOT a transaction verifier or an activation
// path. A future native adapter must authenticate every input/output, enforce
// exclusive reserve spend rules, verify carrier ownership/payout and native
// RingCT proofs, and commit this state with the native DB and exchange ledger.
namespace xgtx_exchange::backing {
struct outpoint {
  id transaction{};
  std::uint32_t index=0;
  bool operator<(const outpoint& b) const {
    return transaction<b.transaction || (transaction==b.transaction && index<b.index);
  }
};
inline bool operator==(const outpoint& a,const outpoint& b){return a.transaction==b.transaction && a.index==b.index;}
struct reserve_state {
  id chain{};
  amount locked=0;
  std::map<outpoint,amount> unspent;
  // Retain spent identities: a native output cannot reappear as a new deposit.
  std::set<outpoint> spent;
  std::set<id> transactions;
  bool operator==(const reserve_state& b) const {
    if(chain!=b.chain || locked!=b.locked || spent!=b.spent || transactions!=b.transactions || unspent.size()!=b.unspent.size())return false;
    auto a=unspent.begin(),c=b.unspent.begin();
    for(;a!=unspent.end();++a,++c)if(a->first<c->first || c->first<a->first || a->second!=c->second)return false;
    return true;
  }
};
struct effect {
  id chain{},transaction{};
  std::vector<outpoint> inputs;
  std::vector<std::pair<std::uint32_t,amount>> outputs;
};
inline void require(bool ok,const char* why){if(!ok)throw std::invalid_argument(why);}
inline amount narrow(wide n){require(n<=std::numeric_limits<amount>::max(),"native reserve overflow");return static_cast<amount>(n);}
inline void invariants(const reserve_state& s,amount liabilities){
  require(s.chain!=id{},"missing native reserve chain");
  require(s.unspent.size()<=1000000 && s.spent.size()<=1000000 && s.transactions.size()<=1000000,"native reserve entry cap");
  wide sum=0;
  for(const auto& entry:s.unspent){
    require(entry.first.transaction!=id{} && entry.second && !s.spent.count(entry.first),"invalid reserve output");
    require(s.transactions.count(entry.first.transaction),"unrecorded reserve output");
    sum+=entry.second;narrow(sum);
  }
  for(const auto& point:s.spent)require(point.transaction!=id{} && s.transactions.count(point.transaction),"unrecorded spent reserve output");
  require(narrow(sum)==s.locked && s.locked==liabilities,"native backing/liability mismatch");
}
// before/after liabilities MUST include all public XGTX balances, pool reserves
// and treasury balances, without double-counting LP shares. Fees paid to miners
// are outside these liabilities. Unsigned or unverified RPC claims must never
// be passed to this function. Values here are atomic units, never floating point.
inline void apply(reserve_state& original,const effect& e,amount before,amount after){
  invariants(original,before);
  require(e.chain==original.chain && e.transaction!=id{},"wrong native reserve transaction domain");
  require(!original.transactions.count(e.transaction),"native reserve transaction replay");
  require(e.inputs.size()<=16 && e.outputs.size()<=16 && (!e.inputs.empty() || !e.outputs.empty()),"native reserve effect size");
  reserve_state candidate=original;
  wide consumed=0,created=0;
  std::set<outpoint> inputs;
  for(const auto& point:e.inputs){
    require(inputs.insert(point).second,"duplicate reserve input");
    auto it=candidate.unspent.find(point);require(it!=candidate.unspent.end(),"missing or spent reserve input");
    consumed+=it->second;narrow(consumed);
    candidate.unspent.erase(it);require(candidate.spent.insert(point).second,"reserve double spend");
  }
  for(const auto& output:e.outputs){
    require(output.first<16 && output.second,"invalid new reserve output");
    outpoint point{e.transaction,output.first};
    require(!candidate.spent.count(point) && candidate.unspent.emplace(point,output.second).second,"duplicate reserve output");
    created+=output.second;narrow(created);
  }
  // Exact net movement; no unsigned subtraction and no credit for miner fees.
  const wide locked=wide(before)-consumed+created;
  candidate.locked=narrow(locked);
  require(candidate.locked==after,"native reserve net movement differs from ledger change");
  candidate.transactions.insert(e.transaction);
  invariants(candidate,after);
  original=std::move(candidate);
}
}
