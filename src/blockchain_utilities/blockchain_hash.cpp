#include <iostream>
#include <memory>
#include <string>

#include <boost/filesystem/path.hpp>
#include <boost/program_options.hpp>

#include "common/command_line.h"
#include "cryptonote_core/cryptonote_core.h"
#include "version.h"

namespace po = boost::program_options;
using namespace cryptonote;

int main(int argc, char* argv[])
{
  TRY_ENTRY();

  epee::string_tools::set_module_name_and_folder(argv[0]);
  tools::on_startup();

  po::options_description desc("Allowed options");

  const command_line::arg_descriptor<uint64_t> arg_height =
    {"height", "Block height to print", 0};

  command_line::add_arg(desc, command_line::arg_help);
  command_line::add_arg(desc, cryptonote::arg_data_dir);
  command_line::add_arg(desc, cryptonote::arg_testnet_on);
  command_line::add_arg(desc, cryptonote::arg_stagenet_on);
  command_line::add_arg(desc, cryptonote::arg_regtest_on);
  command_line::add_arg(desc, arg_height);

  po::variables_map vm;

  bool r = command_line::handle_error_helper(desc, [&]()
  {
    auto parser = po::command_line_parser(argc, argv).options(desc);
    po::store(parser.run(), vm);
    po::notify(vm);
    return true;
  });

  if (!r)
    return 1;

  if (command_line::get_arg(vm, command_line::arg_help))
  {
    std::cout << desc << std::endl;
    return 0;
  }

  const std::string data_dir =
    command_line::get_arg(vm, cryptonote::arg_data_dir);

  const uint64_t height =
    command_line::get_arg(vm, arg_height);

  BlockchainDB* db = new_db();

  if (db == nullptr)
  {
    std::cerr << "Failed to create LMDB database object" << std::endl;
    return 1;
  }

  const std::string filename =
    (boost::filesystem::path(data_dir) / db->get_db_name()).string();

  try
  {
    db->open(filename, DBF_RDONLY);

    const uint64_t chain_height = db->height();

    if (height >= chain_height)
    {
      std::cerr
        << "Requested height " << height
        << " is outside blockchain. Chain height is "
        << chain_height << std::endl;

      db->close();
      delete db;
      return 1;
    }

    const crypto::hash hash =
      db->get_block_hash_from_height(height);

    const cryptonote::blobdata bd =
      db->get_block_blob_from_height(height);

    cryptonote::block blk;

    if (!cryptonote::parse_and_validate_block_from_blob(bd, blk))
    {
      std::cerr << "Failed to parse block at height "
                << height << std::endl;
      db->close();
      delete db;
      return 1;
    }

    std::cout
      << "height: " << height << std::endl
      << "hash: " << hash << std::endl
      << "major_version: " << static_cast<unsigned>(blk.major_version) << std::endl
      << "minor_version: " << static_cast<unsigned>(blk.minor_version) << std::endl
      << "timestamp: " << blk.timestamp << std::endl
      << "prev_id: " << blk.prev_id << std::endl
      << "nonce: " << blk.nonce << std::endl;

    db->close();
    delete db;
  }
  catch (const std::exception& e)
  {
    std::cerr << "Error: " << e.what() << std::endl;
    delete db;
    return 1;
  }

  return 0;

  CATCH_ENTRY("Block hash query error", 1);
}
