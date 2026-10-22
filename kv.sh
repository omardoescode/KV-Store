#!/usr/bin/env bash
# Usage: ./kv.sh get <key> | put <key> <value> | del <key>
URL="${KV_URL:-http://127.0.0.1:7890}/kv"

case "$1" in
  get) curl -i "$URL/$2" ;;
  put) curl -i -X PUT "$URL/$2" -d "{\"value\":\"$3\"}" ;;
  del) curl -i -X DELETE "$URL/$2" ;;
  *)   echo "usage: $0 get <key> | put <key> <value> | del <key>" >&2; exit 1 ;;
esac