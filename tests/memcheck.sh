#!/bin/sh
# Runs tests/http_test.sh with the server under valgrind; fails on a memory error or a definite leak.
set -eu

log=$(mktemp)
trap 'rm -f "$log"' EXIT
export KV_LISTEN_ADDR="${KV_LISTEN_ADDR:-127.0.0.1:7890}"
KV_SERVER="valgrind --leak-check=full --errors-for-leak-kinds=definite --log-file=$log ./build/app" \
  tests/http_test.sh || { cat "$log" >&2; exit 1; }
grep -q 'ERROR SUMMARY: 0 errors' "$log" || { cat "$log" >&2; exit 1; }
echo "memcheck: OK"
