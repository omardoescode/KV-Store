#!/bin/sh
# Runs the server under valgrind, sends requests, and fails on a wrong status code,
# a memory error, a definite leak, or an unclean SIGTERM shutdown.
set -eu

export KV_LISTEN_ADDR="${KV_LISTEN_ADDR:-127.0.0.1:7890}"
export KV_LOG_PATH="${KV_LOG_PATH:-kv.log}"
export KV_DB_PATH="${KV_DB_PATH:-kv.db}"
url=http://$KV_LISTEN_ADDR
log=$(mktemp)

valgrind --leak-check=full --errors-for-leak-kinds=definite --log-file="$log" ./build/app >/dev/null &
pid=$!
trap 'kill "$pid" 2>/dev/null || true; rm -f "$log"' EXIT

tries=0
until curl -sf -o /dev/null "$url/ping"; do
  tries=$((tries + 1))
  if [ "$tries" -ge 60 ] || ! kill -0 "$pid" 2>/dev/null; then
    echo "memcheck: server did not start" >&2
    cat "$log" >&2
    exit 1
  fi
  sleep 1
done

expect() {
  want=$1
  shift
  got=$(curl -s -o /dev/null -w '%{http_code}' "$@")
  [ "$got" = "$want" ] || { echo "memcheck: want $want, got $got: curl $*" >&2; exit 1; }
}

for _ in 1 2 3; do
  # Ping and routing
  expect 200 "$url/ping"
  expect 404 -X PUT "$url/ping"
  expect 404 "$url/no-such-path"

  # Normal lifecycle: put, get, overwrite, get, delete, then it's gone
  expect 200 -X PUT -d '{"value":"hello"}' "$url/kv/test"
  expect 200 "$url/kv/test"
  expect 200 -X PUT -d '{"value":"world"}' "$url/kv/test"
  expect 200 "$url/kv/test"
  expect 200 -X DELETE "$url/kv/test"
  expect 404 "$url/kv/test"
  expect 404 -X DELETE "$url/kv/test"

  # Bad input to PUT
  expect 400 -X PUT -d '{"value":5}' "$url/kv/test"
  expect 400 -X PUT -d 'not json' "$url/kv/test"
  expect 400 -X PUT "$url/kv/test"

  # Missing key in the path matches no route
  expect 404 "$url/kv"
  expect 404 "$url/kv/"

  # Each path segment is percent-decoded after the split
  expect 200 -X PUT -d '{"value":"slash"}' "$url/kv/a%2Fb"
  expect 200 "$url/kv/a%2Fb"
  expect 404 "$url/kv/a/b"
  expect 200 -X DELETE "$url/kv/a%2fb"
  expect 400 "$url/kv/a%zz"
  expect 400 "$url/kv/a%4"
done

kill -TERM "$pid"
status=0
wait "$pid" || status=$?
[ "$status" = 0 ] || { echo "memcheck: exit status $status after SIGTERM, want 0" >&2; cat "$log" >&2; exit 1; }

grep -q 'ERROR SUMMARY: 0 errors' "$log" || { cat "$log" >&2; exit 1; }
echo "memcheck: OK"