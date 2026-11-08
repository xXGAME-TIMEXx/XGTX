#pragma once
#include "engine.hpp"
#include "cryptonote_basic/cryptonote_basic_impl.h"
#include <cstring>

namespace xgtx_exchange {
// User-designated public native-mainnet address. This is configuration source,
// not an activated consensus rule, native bridge, or automatic wallet payout.
inline constexpr char designated_fee_address[] =
  "CmS3B2SaPyCfwGAB7bsaqm6jT4qd5MHkPByjW5Db5jECQ7YkKw9A69rjAe8LZaXfHjQuyrybaKTa5S9ctmXnFQ3H7Y3ETjF";
struct fee_recipient { std::string address; id spend{},view{}; };
inline fee_recipient parse_fee_recipient(cryptonote::network_type network,const std::string& address){
  if(network!=cryptonote::MAINNET)throw std::invalid_argument("mainnet recipient cannot configure another network");
  cryptonote::address_parse_info info{};
  if(!cryptonote::get_account_address_from_str(info,network,address) || info.is_subaddress || info.has_payment_id)
    throw std::invalid_argument("expected valid standard mainnet fee address");
  if(cryptonote::get_account_address_as_str(network,false,info.address)!=address)
    throw std::invalid_argument("noncanonical fee address");
  fee_recipient result;result.address=address;
  static_assert(sizeof(info.address.m_spend_public_key)==32);
  static_assert(sizeof(info.address.m_view_public_key)==32);
  std::memcpy(result.spend.data(),&info.address.m_spend_public_key,32);
  std::memcpy(result.view.data(),&info.address.m_view_public_key,32);
  return result;
}
inline config designated_fee_config(const id& chain,amount activation_height){
  if(chain==id{} || !activation_height)throw std::invalid_argument("explicit chain and future activation height required");
  auto recipient=parse_fee_recipient(cryptonote::MAINNET,designated_fee_address);
  return config{chain,recipient.spend,activation_height,1024};
}
}
