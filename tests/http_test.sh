#!/bin/sh
# Starts the server, sends requests, and fails on a wrong status or body, a forged log line,
# or an unclean SIGTERM shutdown. KV_SERVER overrides the server command (memcheck.sh sets it).
set -eu

dir=$(mktemp -d)
export KV_LISTEN_ADDR="${KV_LISTEN_ADDR:-127.0.0.1:7892}"
export KV_LOG_PATH="$dir/kv.log"
export KV_DB_PATH="$dir/kv.db"
url=http://$KV_LISTEN_ADDR

# shellcheck disable=SC2086 # KV_SERVER is a command line
${KV_SERVER:-./build/app} >/dev/null &
pid=$!
trap 'kill "$pid" 2>/dev/null || true; rm -rf "$dir"' EXIT

tries=0
until [ "$(curl -s -o /dev/null -w '%{http_code}' "$url/")" != 000 ]; do
  tries=$((tries + 1))
  if [ "$tries" -ge 60 ] || ! kill -0 "$pid" 2>/dev/null; then
    echo "http_test: server did not start" >&2
    exit 1
  fi
  sleep 1
done

expect() {
  want=$1
  shift
  got=$(curl -s -o /dev/null -w '%{http_code}' "$@")
  [ "$got" = "$want" ] || { echo "http_test: want $want, got $got: curl $*" >&2; exit 1; }
}

expect_body() {
  want=$1
  shift
  got=$(curl -s "$@")
  [ "$got" = "$want" ] || { echo "http_test: want body $want, got $got: curl $*" >&2; exit 1; }
}

for _ in 1 2 3; do
  # Routing
  expect 404 -X PUT "$url/test"
  expect 404 "$url/no-such-path"

  # Normal lifecycle: put, get, overwrite, get, delete, then it's gone
  expect 200 -X POST -d '{"value":"hello"}' "$url/test"
  expect 200 "$url/test"
  expect_body '{"value":"hello"}' "$url/test"
  expect 200 -X POST -d '{"value":"world"}' "$url/test"
  expect 200 "$url/test"
  expect_body '{"value":"world"}' "$url/test"
  expect 200 -X POST -d '{"value":""}' "$url/test"
  expect_body '{"value":""}' "$url/test"
  expect 200 -X DELETE "$url/test"
  expect 404 "$url/test"
  expect 404 -X DELETE "$url/test"

  # Bad input to POST
  expect 400 -X POST -d '{"value":5}' "$url/test"
  expect 400 -X POST -d 'not json' "$url/test"
  expect 400 -X POST "$url/test"

  # Missing key in the path matches no route
  expect 404 "$url/"

  # Each path segment is percent-decoded after the split
  expect 200 -X POST -d '{"value":"slash"}' "$url/a%2Fb"
  expect 200 "$url/a%2Fb"
  expect 404 "$url/a/b"
  expect 200 -X DELETE "$url/a%2fb"
  expect 400 "$url/a%zz"
  expect 400 "$url/a%4"

  # A NUL byte in a key would cut it short, so "a%00b" must not reach key "a"
  expect 200 -X POST -d '{"value":"a"}' "$url/a"
  expect 400 "$url/a%00b"
  expect 400 -X POST -d '{"value":"x"}' "$url/a%00b"
  expect 400 -X DELETE "$url/a%00b"
  expect_body '{"value":"a"}' "$url/a"
  expect 200 -X DELETE "$url/a"

  # A key must be UTF-8: the key column is SQLite TEXT
  expect 400 -X POST -d '{"value":"x"}' "$url/%FF"
  expect 400 "$url/%C3"
  expect 400 -X DELETE "$url/%ED%A0%80"
  expect 400 "$url/%C0%AF"
  expect 400 "$url/%F4%90%80%80"
  expect 404 "$url/%F0%9F%98%80"
  expect 200 -X POST -d '{"value":"e"}' "$url/%C3%A9"
  expect_body '{"value":"e"}' "$url/%C3%A9"
  expect 200 -X DELETE "$url/%C3%A9"

  # A decoded newline in the path must not start a new line in the log
  expect 404 "$url/x%0A2026-01-01T00:00:00.000%20ERROR%20forged"
done

kill -TERM "$pid"
status=0
wait "$pid" || status=$?
[ "$status" = 0 ] || { echo "http_test: exit status $status after SIGTERM, want 0" >&2; exit 1; }

! grep -q '^2026-01-01T00:00:00.000 ERROR forged' "$KV_LOG_PATH" ||
  { echo "http_test: a request path forged a log line" >&2; exit 1; }
echo "http_test: OK"