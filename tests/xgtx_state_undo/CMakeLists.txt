monero_add_minimal_executable(xgtx_state_undo_tests
  test_state_undo.cpp
  ../../src/xgtx_exchange/engine.cpp)
target_compile_features(xgtx_state_undo_tests PRIVATE cxx_std_17)
target_compile_definitions(xgtx_state_undo_tests PRIVATE XGTX_TEST_CRYPTONOTE=1)
target_link_libraries(xgtx_state_undo_tests PRIVATE cncrypto lmdb_lib ${EXTRA_LIBRARIES})
add_test(NAME xgtx_state_undo COMMAND xgtx_state_undo_tests)
