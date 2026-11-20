#!/bin/sh
# Runs tests/http_test.sh with the server under valgrind, then a few cp tests with cp under
# valgrind; fails on a memory error or a definite leak.
set -eu

dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
vg="valgrind --leak-check=full --errors-for-leak-kinds=definite --child-silent-after-fork=yes"

check() {
  for log in "$@"; do
    grep -q 'ERROR SUMMARY: 0 errors' "$log" || { cat "$log" >&2; exit 1; }
  done
}

export KV_LISTEN_ADDR="${KV_LISTEN_ADDR:-127.0.0.1:7890}"
KV_SERVER="$vg --log-file=$dir/app.log ./build/app" tests/http_test.sh || {
  cat "$dir/app.log" >&2
  exit 1
}
check "$dir/app.log"

KV_CP_WRAPPER="$vg --log-file=$dir/cp.%p.log" python3 tests/cp_test.py \
  PublicApiTest ReplicationTest.test_write_goes_to_every_replica_with_one_version \
  FailureTest.test_dead_node_is_respawned_and_repaired_before_reads \
  JoinTest.test_manual_join_moves_keys_without_losing_writes 2>"$dir/cp_test.out" || {
  cat "$dir/cp_test.out" "$dir"/cp.*.log >&2
  exit 1
}
check "$dir"/cp.*.log
echo "memcheck: OK"
