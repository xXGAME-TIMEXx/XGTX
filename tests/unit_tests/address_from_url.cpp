// Copyright (c) 2014-2024, The Monero Project
// 
// All rights reserved.
// 
// Redistribution and use in source and binary forms, with or without modification, are
// permitted provided that the following conditions are met:
// 
// 1. Redistributions of source code must retain the above copyright notice, this list of
//    conditions and the following disclaimer.
// 
// 2. Redistributions in binary form must reproduce the above copyright notice, this list
//    of conditions and the following disclaimer in the documentation and/or other
//    materials provided with the distribution.
// 
// 3. Neither the name of the copyright holder nor the names of its contributors may be
//    used to endorse or promote products derived from this software without specific
//    prior written permission.
// 
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
// THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
// THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// FIXME: move this into a full wallet2 unit test suite, if possible

#include "gtest/gtest.h"

#include "wallet/wallet2.h"
#include "common/dns_utils.h"
#include "simplewallet/simplewallet.h"
#include <string>

TEST(AddressFromTXT, Success)
{
  std::string addr = "CikxgENAV69Djkf9i1ucbbTsE8rycM5v8AS8mUYnvMS9AjLXmBDvszyY9vBdaximKXQT8wy4NM4GM7WXEFX2Upw76kbqJqN";

  std::string txtr = "oa1:xgtx";
  txtr += " recipient_address=";
  txtr += addr;
  txtr += ";";

  std::string res = tools::dns_utils::address_from_txt_record(txtr);

  EXPECT_STREQ(addr.c_str(), res.c_str());

  std::string txtr2 = "foobar";

  txtr2 += txtr;

  txtr2 += "more foobar";

  res = tools::dns_utils::address_from_txt_record(txtr2);

  EXPECT_STREQ(addr.c_str(), res.c_str());

  std::string txtr3 = "foobar oa1:xgtx tx_description=\"XGTX payment\"; ";
  txtr3 += "recipient_address=";
  txtr3 += addr;
  txtr3 += "; foobar";

  res = tools::dns_utils::address_from_txt_record(txtr3);

  EXPECT_STREQ(addr.c_str(), res.c_str());
}

TEST(AddressFromTXT, Failure)
{
  std::string txtr = "oa1:xgtx recipient_address=not a real address";

  std::string res = tools::dns_utils::address_from_txt_record(txtr);

  ASSERT_STREQ("", res.c_str());

  txtr += ";";

  res = tools::dns_utils::address_from_txt_record(txtr);
  ASSERT_STREQ("", res.c_str());
}


TEST(AddressFromURL, Disabled)
{
  cryptonote::address_parse_info info;
  tools::wallet2 wallet;
  wallet.enable_dns(false);

  EXPECT_TRUE(cryptonote::get_account_address_from_str_or_url(
      info, cryptonote::MAINNET, "CmS3B2SaPyCfwGAB7bsaqm6jT4qd5MHkPByjW5Db5jECQ7YkKw9A69rjAe8LZaXfHjQuyrybaKTa5S9ctmXnFQ3H7Y3ETjF", wallet.is_dns_enabled()));
  EXPECT_FALSE(cryptonote::get_account_address_from_str_or_url(
      info, cryptonote::MAINNET, "invalid.xgtx", wallet.is_dns_enabled()));

  wallet.enable_dns(true);
  wallet.set_offline();
  EXPECT_FALSE(wallet.is_dns_enabled());
}
