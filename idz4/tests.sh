#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
make clean all >/dev/null
tmp=$(mktemp -d)
port=$((32000 + $$ % 20000))
pid=
cleanup(){ [[ -z "$pid" ]] || { kill -INT "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; }; rm -rf "$tmp"; }
trap cleanup EXIT

# READ_OLD: reader must continue receiving the previous version during preparation.
./bin/idz4 server --bind 127.0.0.1 --port "$port" --strategy READ_OLD --prep-ms 300 >"$tmp/server1" 2>&1 & pid=$!
sleep .2
./bin/idz4 writer --host 127.0.0.1 --port "$port" --id 2 --ops 1 >"$tmp/writer" & wp=$!
sleep .1
./bin/idz4 reader --host 127.0.0.1 --port "$port" --id 1 --ops 2 >"$tmp/reader"
wait "$wp"
./bin/idz4 observer --host 127.0.0.1 --port "$port" --id 9 --ops 1 >"$tmp/observer"
kill -INT "$pid"; wait "$pid"; pid=
grep -q 'DATA 1 ' "$tmp/reader"
grep -q 'начал подготовку версии 2' "$tmp/server1"
grep -q 'версия=1' "$tmp/server1"
grep -q 'OK PUBLISHED 2' "$tmp/writer"
grep -q 'STATUS version=2' "$tmp/observer"

# WAIT_NEW: reader must wait until the writer publishes version 2.
port=$((port+1))
./bin/idz4 server --bind 127.0.0.1 --port "$port" --strategy WAIT_NEW --prep-ms 300 >"$tmp/server2" 2>&1 & pid=$!
sleep .2
./bin/idz4 writer --host 127.0.0.1 --port "$port" --id 3 --ops 1 >"$tmp/writer2" & wp=$!
sleep .1
./bin/idz4 reader --host 127.0.0.1 --port "$port" --id 4 --ops 1 >"$tmp/reader2" & rp=$!
wait "$wp"; wait "$rp"
kill -INT "$pid"; wait "$pid"; pid=
grep -q 'ожидает публикации' "$tmp/server2"
grep -q 'DATA 2 ' "$tmp/reader2"
echo "IDZ4 integration tests passed."
