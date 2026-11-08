#!/usr/bin/env bash
set -euo pipefail
task_root="$(cd "$(dirname "$0")/../.." && pwd)"
task_build="$(mktemp -d)"
trap 'rm -rf "$task_build"' EXIT
task_lmdb="$task_root/external/db_drivers/liblmdb"
task_flags=(-O1 -g)
task_link=()
if [[ "${XGTX_SANITIZE:-0}" == 1 ]]; then
  task_flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
  task_link+=(-fsanitize=address,undefined)
fi
gcc "${task_flags[@]}" -c "$task_lmdb/mdb.c" -o "$task_build/mdb.o"
gcc "${task_flags[@]}" -c "$task_lmdb/midl.c" -o "$task_build/midl.o"
g++ -std=c++17 "${task_flags[@]}" -Wall -Wextra -Werror -I"$task_lmdb" \
  "$task_root/src/xgtx_exchange/engine.cpp" \
  "$task_root/tests/xgtx_exchange/test_engine.cpp" \
  "$task_build/mdb.o" "$task_build/midl.o" -lcrypto -pthread \
  "${task_link[@]}" -o "$task_build/test_engine"
"$task_build/test_engine"
