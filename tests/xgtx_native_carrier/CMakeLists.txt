monero_add_minimal_executable(xgtx_native_carrier_tests
  test_native_carrier.cpp
  ../../src/xgtx_exchange/engine.cpp)
target_compile_features(xgtx_native_carrier_tests PRIVATE cxx_std_17)
target_link_libraries(xgtx_native_carrier_tests PRIVATE cryptonote_basic ${EXTRA_LIBRARIES})
add_test(NAME xgtx_native_carrier COMMAND xgtx_native_carrier_tests)
