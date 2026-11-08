#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "serialization/keyvalue_serialization.h"
namespace cryptonote {
// Monetary values and nonces are canonical decimal strings to preserve all
// uint64 digits in browser clients. IDs are 32-byte lowercase hexadecimal.
struct xgtx_rpc_asset {
  std::string id,issuer,supply,ticker,balance;std::uint8_t decimals=0;
  BEGIN_KV_SERIALIZE_MAP()
    KV_SERIALIZE(id) KV_SERIALIZE(issuer) KV_SERIALIZE(supply)
    KV_SERIALIZE(ticker) KV_SERIALIZE(decimals) KV_SERIALIZE(balance)
  END_KV_SERIALIZE_MAP()
};
struct xgtx_rpc_pool {
  std::string id,asset_a,asset_b,reserve_a,reserve_b,shares,owner_shares;
  BEGIN_KV_SERIALIZE_MAP()
    KV_SERIALIZE(id) KV_SERIALIZE(asset_a) KV_SERIALIZE(asset_b)
    KV_SERIALIZE(reserve_a) KV_SERIALIZE(reserve_b) KV_SERIALIZE(shares) KV_SERIALIZE(owner_shares)
  END_KV_SERIALIZE_MAP()
};
struct COMMAND_RPC_XGTX_GET_EXCHANGE {
  struct request {
    std::string owner,after_asset,after_pool;std::uint32_t limit=32;
    BEGIN_KV_SERIALIZE_MAP()
      KV_SERIALIZE_OPT(owner,std::string()) KV_SERIALIZE_OPT(after_asset,std::string())
      KV_SERIALIZE_OPT(after_pool,std::string()) KV_SERIALIZE_OPT(limit,(std::uint32_t)32)
    END_KV_SERIALIZE_MAP()
  };
  struct response {
    std::string status,chain,tip,treasury,native_locked,next_nonce,next_asset,next_pool,reserve_outputs;
    std::uint64_t height=0,activation_height=0;bool native_backing=false;
    std::uint32_t lp_fee_bps=25,protocol_fee_bps=5;
    std::vector<xgtx_rpc_asset> assets;std::vector<xgtx_rpc_pool> pools;
    BEGIN_KV_SERIALIZE_MAP()
      KV_SERIALIZE(status) KV_SERIALIZE(chain) KV_SERIALIZE(tip) KV_SERIALIZE(treasury)
      KV_SERIALIZE(native_locked) KV_SERIALIZE(reserve_outputs) KV_SERIALIZE(next_nonce) KV_SERIALIZE(next_asset) KV_SERIALIZE(next_pool)
      KV_SERIALIZE(height) KV_SERIALIZE(activation_height) KV_SERIALIZE(native_backing)
      KV_SERIALIZE(lp_fee_bps) KV_SERIALIZE(protocol_fee_bps) KV_SERIALIZE(assets) KV_SERIALIZE(pools)
    END_KV_SERIALIZE_MAP()
  };
};
struct xgtx_rpc_reserve {
  std::string global_index,transaction,prunable_hash,origin,key,commitment;
  std::uint32_t output_index=0;std::uint64_t height=0,unlock_time=0;bool spent=false;
  BEGIN_KV_SERIALIZE_MAP()
    KV_SERIALIZE(global_index) KV_SERIALIZE(transaction) KV_SERIALIZE(prunable_hash)
    KV_SERIALIZE(origin) KV_SERIALIZE(key) KV_SERIALIZE(commitment) KV_SERIALIZE(output_index)
    KV_SERIALIZE(height) KV_SERIALIZE(unlock_time) KV_SERIALIZE(spent)
  END_KV_SERIALIZE_MAP()
};
struct COMMAND_RPC_XGTX_GET_RESERVE_RING {
  struct request {
    std::string start,tip;
    BEGIN_KV_SERIALIZE_MAP() KV_SERIALIZE(start) KV_SERIALIZE(tip) END_KV_SERIALIZE_MAP()
  };
  struct response {
    std::string status,chain,tip,count;std::uint64_t height=0;
    std::vector<xgtx_rpc_reserve> outputs;
    BEGIN_KV_SERIALIZE_MAP()
      KV_SERIALIZE(status) KV_SERIALIZE(chain) KV_SERIALIZE(tip) KV_SERIALIZE(count)
      KV_SERIALIZE(height) KV_SERIALIZE(outputs)
    END_KV_SERIALIZE_MAP()
  };
};

struct COMMAND_RPC_XGTX_QUOTE_SWAP {
  struct request {
    std::string asset_in,asset_out,amount_in;
    BEGIN_KV_SERIALIZE_MAP()
      KV_SERIALIZE(asset_in) KV_SERIALIZE(asset_out) KV_SERIALIZE(amount_in)
    END_KV_SERIALIZE_MAP()
  };
  struct response {
    std::string status,chain,tip,pool,amount_in,amount_out,lp_fee,protocol_fee;
    std::uint64_t height=0;
    BEGIN_KV_SERIALIZE_MAP()
      KV_SERIALIZE(status) KV_SERIALIZE(chain) KV_SERIALIZE(tip) KV_SERIALIZE(pool)
      KV_SERIALIZE(amount_in) KV_SERIALIZE(amount_out) KV_SERIALIZE(lp_fee) KV_SERIALIZE(protocol_fee) KV_SERIALIZE(height)
    END_KV_SERIALIZE_MAP()
  };
};
}
