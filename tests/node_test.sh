#!/bin/sh
# Storage node features for cp: schema upgrade of an old database and the tombstone purge.
set -eu

dir=$(mktemp -d)
export KV_LISTEN_ADDR="${KV_LISTEN_ADDR:-127.0.0.1:7893}"
export KV_LOG_PATH="$dir/kv.log"
export KV_DB_PATH="$dir/kv.db"
export KV_TOMBSTONE_TTL_S=1
url=http://$KV_LISTEN_ADDR
pid=
trap 'kill "$pid" 2>/dev/null || true; rm -rf "$dir"' EXIT

fail() {
  echo "node_test: $*" >&2
  exit 1
}

start() {
  # shellcheck disable=SC2086 # KV_SERVER is a command line
  ${KV_SERVER:-./build/app} >/dev/null &
  pid=$!
  tries=0
  until [ "$(curl -s -o /dev/null -w '%{http_code}' "$url/_internal/health")" = 200 ]; do
    tries=$((tries + 1))
    if [ "$tries" -ge 60 ] || ! kill -0 "$pid" 2>/dev/null; then
      fail "server did not start"
    fi
    sleep 0.2
  done
}

stop() {
  kill -TERM "$pid"
  wait "$pid" || fail "unclean stop"
}

expect_body() {
  want=$1
  shift
  got=$(curl -s "$@")
  [ "$got" = "$want" ] || fail "want body $want, got $got: curl $*"
}

# A database from before versions loads with version 0 and no tombstones
sqlite3 "$KV_DB_PATH" "CREATE TABLE kv(key TEXT PRIMARY KEY, value BLOB) WITHOUT ROWID;
  INSERT INTO kv VALUES('old', CAST('v' AS BLOB));"
start
expect_body '{"value":"v"}' "$url/old"
expect_body '{"items":[{"key":"old","value":"v","version":0,"deleted":false}],"next":null}' \
  "$url/_internal/dump"

# A paused purge keeps tombstones past the TTL; a resumed one removes them
curl -sf -X POST -d '{"paused":true}' "$url/_internal/purge"
curl -sf -X DELETE "$url/old"
sleep 2.5
expect_body '{"items":[{"key":"old","value":"","version":0,"deleted":true}],"next":null}' \
  "$url/_internal/dump"
curl -sf -X POST -d '{"paused":false}' "$url/_internal/purge"
sleep 2.5
expect_body '{"items":[],"next":null}' "$url/_internal/dump"
expect_body 'Key not found.' -X DELETE "$url/old"
stop

echo "node_test: OK"
