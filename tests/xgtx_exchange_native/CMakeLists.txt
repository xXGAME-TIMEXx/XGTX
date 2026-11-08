# Opt-in test target only. Does not link the exchange into daemon consensus.
monero_add_minimal_executable(xgtx_exchange_native_tests
  ../xgtx_exchange/test_engine.cpp
  ../../src/xgtx_exchange/engine.cpp)
target_compile_features(xgtx_exchange_native_tests PRIVATE cxx_std_17)
target_compile_definitions(xgtx_exchange_native_tests PRIVATE XGTX_TEST_CRYPTONOTE=1)
target_link_libraries(xgtx_exchange_native_tests PRIVATE cncrypto lmdb_lib ${EXTRA_LIBRARIES})
add_test(NAME xgtx_exchange_native COMMAND xgtx_exchange_native_tests)
