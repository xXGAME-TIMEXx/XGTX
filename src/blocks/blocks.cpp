#include "blocks.h"

#include <unordered_map>

extern const unsigned char checkpoints[];
extern const size_t checkpoints_len;
extern const unsigned char stagenet_blocks[];
extern const size_t stagenet_blocks_len;
extern const unsigned char testnet_blocks[];
extern const size_t testnet_blocks_len;

namespace blocks
{

  const epee::span<const unsigned char> GetCheckpointsData(cryptonote::network_type network)
  {
    // XGTX has its own blockchain history. Do not load the inherited
    // Monero per-block fast-sync checkpoint datasets.
    return nullptr;
  }

}
