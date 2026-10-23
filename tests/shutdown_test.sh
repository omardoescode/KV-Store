#!/bin/sh
# Starts the server, PUTs a value, stops it with a signal at once, restarts it, and fails if
# the value is gone. The long flush interval leaves the value to the shutdown flush.
# Then checks that a locked database makes the stop fail fast with a nonzero status, and that a
# second signal ends a stop that waits on an open request.
set -eu

dir=$(mktemp -d)
export KV_LISTEN_ADDR="${KV_LISTEN_ADDR:-127.0.0.1:7891}"
export KV_LOG_PATH="$dir/kv.log"
export KV_DB_PATH="$dir/kv.db"
export KV_FLUSH_INTERVAL_MS=3600000
url=http://$KV_LISTEN_ADDR
pid=
helper=
trap '[ -z "$pid" ] || kill -KILL "$pid" 2>/dev/null || true; [ -z "$helper" ] || kill "$helper" 2>/dev/null || true; rm -rf "$dir"' EXIT

start() {
  ./build/app >/dev/null &
  pid=$!
  tries=0
  until curl -sf -o /dev/null "$url/ping"; do
    tries=$((tries + 1))
    if [ "$tries" -ge 50 ] || ! kill -0 "$pid" 2>/dev/null; then
      echo "shutdown_test: server did not start" >&2
      exit 1
    fi
    sleep 0.1
  done
}

# stop SIG WANT_STATUS SECONDS: send SIG, fail unless the server exits with WANT_STATUS in SECONDS.
stop() {
  kill "-$1" "$pid"
  tries=0
  while kill -0 "$pid" 2>/dev/null; do
    tries=$((tries + 1))
    [ "$tries" -le "$(($3 * 10))" ] || { echo "shutdown_test: still running ${3}s after SIG$1" >&2; exit 1; }
    sleep 0.1
  done
  status=0
  wait "$pid" || status=$?
  pid=
  [ "$status" = "$2" ] || { echo "shutdown_test: SIG$1 exit status $status, want $2" >&2; exit 1; }
}

for sig in TERM INT; do
  start
  got=$(curl -s -o /dev/null -w '%{http_code}' -X PUT -d "{\"value\":\"$sig\"}" "$url/kv/k")
  [ "$got" = 200 ] || { echo "shutdown_test: PUT got $got" >&2; exit 1; }
  stop "$sig" 0 5

  start
  body=$(curl -s "$url/kv/k")
  stop TERM 0 5
  case $body in
  *"\"$sig\""*) ;;
  *) echo "shutdown_test: after SIG$sig restart, GET /kv/k = $body" >&2; exit 1 ;;
  esac
done

start
curl -s -o /dev/null -X PUT -d '{"value":"lost"}' "$url/kv/lost"
(echo 'BEGIN IMMEDIATE;'; sleep 10) | sqlite3 "$KV_DB_PATH" &
helper=$!
sleep 0.5
stop TERM 1 5
kill "$helper" 2>/dev/null || true
helper=

start
head -c 100 /dev/zero | curl -s -o /dev/null -X PUT --limit-rate 1 --data-binary @- "$url/kv/slow" &
helper=$!
sleep 0.5
kill -TERM "$pid"
sleep 1
kill -0 "$pid" 2>/dev/null || { echo "shutdown_test: stopped before the open request ended" >&2; exit 1; }
stop INT 1 2
kill "$helper" 2>/dev/null || true
helper=
echo "shutdown_test: OK"
