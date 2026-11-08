monero_add_minimal_executable(xgtx_fee_recipient_tests
  test_fee_recipient.cpp
  ../../src/xgtx_exchange/engine.cpp)
target_compile_features(xgtx_fee_recipient_tests PRIVATE cxx_std_17)
target_link_libraries(xgtx_fee_recipient_tests PRIVATE cryptonote_basic ${EXTRA_LIBRARIES})
add_test(NAME xgtx_fee_recipient COMMAND xgtx_fee_recipient_tests)
