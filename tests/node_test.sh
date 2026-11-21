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

# A database with LWW versions loads each version as the ts of an empty clock; a write with a
# clock dominates it
sqlite3 "$KV_DB_PATH" "CREATE TABLE kv(key TEXT PRIMARY KEY, value BLOB,
  version INTEGER NOT NULL DEFAULT 0, deleted INTEGER NOT NULL DEFAULT 0) WITHOUT ROWID;
  INSERT INTO kv VALUES('lww', CAST('v' AS BLOB), 42, 0);"
start
expect_body '{"value":"v","clock":"","ts":42,"coord":0,"deleted":false}' "$url/_internal/get?key=lww"
curl -sf -X POST -H 'x-kv-clock: 2:1' -H 'x-kv-ts: 7' -H 'x-kv-coord: 2' -d '{"value":"new"}' \
  "$url/lww"
curl -sf -X POST "$url/_internal/flush"
expect_body '{"items":[{"key":"lww","value":"new","clock":"2:1","ts":7,"coord":2,"deleted":false}],"next":null}' \
  "$url/_internal/dump"
stop
rm -f "$KV_DB_PATH"*

# A database from before versions loads with version 0 and no tombstones
sqlite3 "$KV_DB_PATH" "CREATE TABLE kv(key TEXT PRIMARY KEY, value BLOB) WITHOUT ROWID;
  INSERT INTO kv VALUES('old', CAST('v' AS BLOB));"
start
expect_body '{"value":"v"}' "$url/old"
expect_body '{"items":[{"key":"old","value":"v","clock":"","ts":0,"coord":0,"deleted":false}],"next":null}' \
  "$url/_internal/dump"

# A paused purge keeps tombstones past the TTL; a resumed one removes them
curl -sf -X POST -d '{"paused":true}' "$url/_internal/purge"
curl -sf -X DELETE "$url/old"
sleep 2.5
expect_body '{"items":[{"key":"old","value":"","clock":"","ts":0,"coord":0,"deleted":true}],"next":null}' \
  "$url/_internal/dump"
curl -sf -X POST -d '{"paused":false}' "$url/_internal/purge"
sleep 2.5
expect_body '{"items":[],"next":null}' "$url/_internal/dump"
expect_body 'Key not found.' -X DELETE "$url/old"
stop

# Flush commits at once: the dump reads only committed rows
rm -f "$KV_DB_PATH"*
export KV_FLUSH_INTERVAL_MS=60000
start
curl -sf -X POST -d '{"value":"f"}' "$url/fl"
expect_body '{"items":[],"next":null}' "$url/_internal/dump"
curl -sf -X POST "$url/_internal/flush"
expect_body '{"items":[{"key":"fl","value":"f","clock":"","ts":0,"coord":0,"deleted":false}],"next":null}' \
  "$url/_internal/dump"
stop

# With sync ack, a write responds after its commit
rm -f "$KV_DB_PATH"*
export KV_FLUSH_INTERVAL_MS=300 KV_SYNC_ACK=1
start
curl -sf -X POST -d '{"value":"s"}' "$url/sa"
expect_body '{"items":[{"key":"sa","value":"s","clock":"","ts":0,"coord":0,"deleted":false}],"next":null}' \
  "$url/_internal/dump"
curl -sf -X DELETE "$url/sa"
expect_body '{"items":[{"key":"sa","value":"","clock":"","ts":0,"coord":0,"deleted":true}],"next":null}' \
  "$url/_internal/dump"
stop

# Anti-entropy reads: a ring segment's digest changes only when one of its entries changes, and a
# range gives the entries of a segment, tombstones too. End -1 is the top of the ring as int64, so
# one segment holds every key.
rm -f "$KV_DB_PATH"*
start
stamped() {
  curl -sf -X "$1" -H "x-kv-clock: $2" -H "x-kv-ts: $3" -H 'x-kv-coord: 1' "$4" "$5" >/dev/null
}
stamped POST 1:2 2 -d'{"value":"a"}' "$url/ae-a"
stamped POST 1:1 1 -d'{"value":"b"}' "$url/ae-b"
curl -s -o /dev/null -X DELETE -H 'x-kv-clock: 1:2' -H 'x-kv-ts: 2' -H 'x-kv-coord: 1' "$url/ae-b"
digest() { curl -s -X POST -d '{"ends":[-1]}' "$url/_internal/digest"; }
d1=$(digest)
case $d1 in '{"hashes":['*']}') ;; *) fail "bad digest $d1" ;; esac
[ "$d1" != '{"hashes":[0]}' ] || fail "digest of two entries is 0"
stamped POST 1:1 9 -d'{"value":"older"}' "$url/ae-a"
[ "$(digest)" = "$d1" ] || fail "a dominated write changed the digest"
stamped POST 1:3 3 -d'{"value":"newer"}' "$url/ae-a"
[ "$(digest)" != "$d1" ] || fail "a newer write kept the digest"
range=$(curl -s "$url/_internal/range?start=0&end=0")
case $range in *'{"key":"ae-a","value":"newer","clock":"1:3","ts":3,"coord":1,"deleted":false}'*) ;;
*) fail "range misses ae-a: $range" ;; esac
case $range in *'{"key":"ae-b","value":"","clock":"1:2","ts":2,"coord":1,"deleted":true}'*) ;;
*) fail "range misses the tombstone of ae-b: $range" ;; esac
[ "$(echo "$range" | grep -o '"key"' | wc -l)" = 2 ] || fail "range has other keys: $range"
stop

echo "node_test: OK"
