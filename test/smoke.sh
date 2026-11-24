#!/usr/bin/env bash
# End-to-end smoke test: CRUD, MVCC transaction and EXPLAIN.
# Usage: smoke.sh <server-binary> <client-binary>
set -euo pipefail

SERVER="$1"
CLIENT="$2"
PORT=48200
TMP_DIR="$(mktemp -d)"
DB_PATH="$TMP_DIR/edb.data"

"$SERVER" --dbpath "$DB_PATH" --svcname "$PORT" \
   --logpath "$TMP_DIR/diag.log" >"$TMP_DIR/server.out" 2>&1 &
SERVER_PID=$!
trap 'kill "$SERVER_PID" 2>/dev/null || true; rm -rf "$TMP_DIR"' EXIT

for _ in $(seq 1 50); do
   if ( exec 3<>"/dev/tcp/127.0.0.1/$PORT" ) 2>/dev/null; then
      exec 3<&- 3>&-
      break
   fi
   sleep 0.1
done

OUTPUT="$(printf 'connect localhost %s\ninsert {"_id":1,"name":"smoke"}\nquery {"_id":1}\nexplain {"_id":1}\nbegin\ninsert {"_id":2,"name":"rolledback"}\nrollback\nquery {"_id":2}\nquit\n' "$PORT" | "$CLIENT" 2>&1)"
echo "$OUTPUT"

grep -q '{ _id: 1, name: "smoke" }' <<<"$OUTPUT"
grep -q 'ID_INDEX_LOOKUP' <<<"$OUTPUT"
if grep -q '{ _id: 2, name: "rolledback" }' <<<"$OUTPUT"; then
   echo "smoke test failed: rolled-back record is visible"
   exit 1
fi

echo "smoke test passed"
