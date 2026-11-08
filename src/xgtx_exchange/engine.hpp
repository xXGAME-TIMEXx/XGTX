#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace xgtx_exchange {
using id = std::array<std::uint8_t,32>;
using signature = std::array<std::uint8_t,64>;
using bytes = std::vector<std::uint8_t>;
using amount = std::uint64_t;
using wide = unsigned __int128;
inline constexpr id native_asset{};
inline constexpr amount native_reserve_bucket=std::numeric_limits<amount>::max()/16;
enum class kind : std::uint8_t { create=1, transfer=2, burn=3, pool_create=4, liquidity_add=5, liquidity_remove=6, swap=7, native_deposit=8, native_withdraw=9 };
struct operation {
  kind type=kind::create;
  id chain{}, sender{}, asset{}, other{}, recipient{};
  amount nonce=1, deadline=0, quantity=0, limit=0, minimum=0;
  std::uint8_t decimals=0;
  std::string ticker;
};
struct envelope { operation op; signature sig{}; };
struct asset_info { id issuer{}; amount supply=0; std::uint8_t decimals=0; std::string ticker; };
struct pool { id a{},b{}; amount reserve_a=0,reserve_b=0,shares=0; };
struct config { id chain{},treasury{}; amount activation_height=0; std::size_t max_operations=1024; bool native_backing=false; };
struct state {
  amount native_locked=0;
  amount height=0;
  id tip{};
  std::map<id,asset_info> assets;
  std::map<std::pair<id,id>,amount> balances,lp;
  std::map<id,amount> nonces;
  std::map<id,pool> pools;
  bool operator==(const state& rhs) const;
};
struct receipt { id asset{},pool_id{}; amount output=0,lp_fee=0,protocol_fee=0,shares=0,used_a=0,used_b=0; };
struct block_result { state next; std::vector<receipt> receipts; };
using hasher = std::function<id(const bytes&)>;
using verifier = std::function<bool(const id&,const bytes&,const signature&)>;
bytes encode(const operation&);
operation decode(const bytes&);
bytes signing_message(const operation&);
bytes encode_envelope(const envelope&);
envelope decode_envelope(const bytes&);
bytes serialize(const state&);
state deserialize(const bytes&);
// Internal native adapter evidence, NOT a cryptographic proof. The caller must
// authenticate amounts, transaction binding, payout and reserve spend rules.
// Never populate this struct from RPC claims. Only the configured native adapter
// may derive it from authenticated transaction proofs and database records.
struct native_effect { id transaction{},intent{},payout{}; amount locked=0,released=0; };
struct native_block_operation { envelope intent; std::optional<native_effect> effect; };
class engine {
 public:
  engine(config c,hasher hash,verifier verify);
  id asset_id(const operation&) const;
  id pool_id(id a,id b) const;
  receipt quote_swap(const state&,id asset_in,id asset_out,amount quantity) const;
  block_result apply_block(const state&,amount height,const id& parent,const id& block,const std::vector<envelope>&) const;
  receipt apply(state&,const envelope&,amount height) const;
  block_result apply_verified_block(const state&,amount,const id& parent,const id& block,const std::vector<native_block_operation>&) const;
  receipt apply_verified_native(state&,const envelope&,amount height,const native_effect&) const;
  void invariants(const state&) const;
  id state_commitment(const state& s) const {return hash_(serialize(s));}
  const config& settings() const {return cfg_;}
 private:
  config cfg_; hasher hash_; verifier verify_;
  receipt apply_internal(state&,const envelope&,amount,const native_effect*) const;
  receipt transition(state&,const operation&) const;
};
}
