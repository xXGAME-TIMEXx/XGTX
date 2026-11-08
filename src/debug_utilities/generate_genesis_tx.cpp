#include <iostream>
#include <string>

#include "cryptonote_basic/cryptonote_basic_impl.h"
#include "cryptonote_core/cryptonote_tx_utils.h"
#include "cryptonote_basic/cryptonote_format_utils.h"
#include "string_tools.h"

int main(int argc, char* argv[])
{
  if (argc != 3)
  {
    std::cerr << "Usage: generate-genesis-tx <mainnet|testnet|stagenet> <founder-address>\n";
    return 1;
  }

  const std::string network = argv[1];
  const std::string address = argv[2];

  cryptonote::network_type nettype;
  if (network == "mainnet")
    nettype = cryptonote::MAINNET;
  else if (network == "testnet")
    nettype = cryptonote::TESTNET;
  else if (network == "stagenet")
    nettype = cryptonote::STAGENET;
  else
  {
    std::cerr << "Invalid network: " << network << "\n";
    return 1;
  }

  cryptonote::address_parse_info info{};

  if (!cryptonote::get_account_address_from_str(
        info,
        nettype,
        address))
  {
    std::cerr << "Invalid founder address for " << network << "\n";
    return 1;
  }

  cryptonote::transaction tx{};

  if (!cryptonote::construct_miner_tx(
        0,              // height
        0,              // median weight
        0,              // already generated coins
        0,              // current block weight
        0,              // fees
        info.address,   // founder wallet
        tx,
        cryptonote::blobdata(),
        1,
        XGTX_INITIAL_PROTOCOL_VERSION))
  {
    std::cerr << "Failed to construct genesis transaction\n";
    return 1;
  }

  const cryptonote::blobdata blob =
      cryptonote::tx_to_blob(tx);

  std::cout
      << epee::string_tools::buff_to_hex_nodelimer(blob)
      << std::endl;

  return 0;
}
