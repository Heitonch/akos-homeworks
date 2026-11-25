#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
make clean all >/dev/null
tmp=$(mktemp -d)
port=$((32000 + $$ % 20000))
pid=
cleanup(){ [[ -z "$pid" ]] || { kill -INT "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; }; rm -rf "$tmp"; }
trap cleanup EXIT

wait_for_log() {
    local pattern=$1
    for _ in {1..100}; do
        if grep -q "$pattern" logs/idz4_server.log 2>/dev/null; then
            return 0
        fi
        sleep 0.05
    done
    echo "Timed out waiting for server event: $pattern" >&2
    return 1
}

wait_for_server() {
    python3 - "$1" <<'PY'
import socket
import sys
import time

port = int(sys.argv[1])
for _ in range(100):
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=0.2):
            raise SystemExit(0)
    except OSError:
        time.sleep(0.05)
raise SystemExit("server did not start")
PY
}

# READ_OLD: reads during preparation must use the previous confirmed version.
./bin/idz4 server --bind 127.0.0.1 --port "$port" --strategy READ_OLD --prep-ms 1000 --read-ms 0 --confirm-ms 0 >"$tmp/server1" 2>&1 & pid=$!
wait_for_server "$port"
./bin/idz4 writer --host 127.0.0.1 --port "$port" --id 2 --ops 1 --seed 42 >"$tmp/writer" & wp=$!
wait_for_log '\[Writer 2\] начал подготовку версии 2'
./bin/idz4 reader --host 127.0.0.1 --port "$port" --id 1 --ops 2 >"$tmp/reader"
wait "$wp"
./bin/idz4 observer --host 127.0.0.1 --port "$port" --id 9 --ops 1 >"$tmp/observer"
kill -INT "$pid"; wait "$pid"; pid=
grep -q 'DATA 1 ' "$tmp/reader"
grep -q 'начал подготовку версии 2' "$tmp/server1"
grep -q 'версия=1' "$tmp/server1"
grep -q 'OK PUBLISHED 2' "$tmp/writer"
grep -q 'STATUS version=2' "$tmp/observer"

# WAIT_NEW: a reader must receive the newly published version.
port=$((port+1))
./bin/idz4 server --bind 127.0.0.1 --port "$port" --strategy WAIT_NEW --prep-ms 300 --read-ms 0 --confirm-ms 0 >"$tmp/server2" 2>&1 & pid=$!
wait_for_server "$port"
./bin/idz4 writer --host 127.0.0.1 --port "$port" --id 3 --ops 1 >"$tmp/writer2" & wp=$!
wait_for_log '\[Writer 3\] начал подготовку версии 2'
./bin/idz4 reader --host 127.0.0.1 --port "$port" --id 4 --ops 1 >"$tmp/reader2" & rp=$!
wait "$wp"; wait "$rp"
kill -INT "$pid"; wait "$pid"; pid=
grep -q 'ожидает публикации' "$tmp/server2"
grep -q 'DATA 2 ' "$tmp/reader2"

# One-element database and one-stage publication.
port=$((port+1))
./bin/idz4 server --bind 127.0.0.1 --port "$port" --strategy READ_OLD --size 1 --stages 1 --min 1 --max 9 --read-ms 0 --prep-ms 0 --confirm-ms 0 >"$tmp/server3" 2>&1 & pid=$!
wait_for_server "$port"
./bin/idz4 writer --host 127.0.0.1 --port "$port" --id 5 --ops 1 >"$tmp/writer3"
./bin/idz4 reader --host 127.0.0.1 --port "$port" --id 6 --ops 1 >"$tmp/reader3"
kill -INT "$pid"; wait "$pid"; pid=
grep -q 'OK PUBLISHED 2' "$tmp/writer3"
grep -q 'DATA 2 0 ' "$tmp/reader3"

# Reader selection is pseudo-random, reproducible and not a fixed cycle.
port=$((port+1))
./bin/idz4 server --bind 127.0.0.1 --port "$port" --strategy READ_OLD --size 7 --stages 2 --read-ms 0 --prep-ms 0 --confirm-ms 0 >"$tmp/server4" 2>&1 & pid=$!
wait_for_server "$port"
./bin/idz4 reader --host 127.0.0.1 --port "$port" --id 77 --ops 12 --seed 1234 >"$tmp/random1"
./bin/idz4 reader --host 127.0.0.1 --port "$port" --id 77 --ops 12 --seed 1234 >"$tmp/random2"
python3 - "$tmp/random1" "$tmp/random2" <<'PY'
import sys

def indices(path):
    with open(path, encoding="utf-8") as source:
        return [line.split()[4] for line in source if " DATA " in line]

first = indices(sys.argv[1])
second = indices(sys.argv[2])
assert len(first) == 12, first
assert first == second, (first, second)
assert len(set(first)) > 1, first
PY
kill -INT "$pid"; wait "$pid"; pid=

# Reject malformed parameters before opening a listening socket.
if ./bin/idz4 server --port 70000 >"$tmp/invalid-port" 2>&1; then
    echo "Invalid port was accepted" >&2; exit 1
fi
if ./bin/idz4 server --stages -1 >"$tmp/invalid-stages" 2>&1; then
    echo "Invalid stage count was accepted" >&2; exit 1
fi
if ./bin/idz4 reader --port abc >"$tmp/invalid-client-port" 2>&1; then
    echo "Invalid client port was accepted" >&2; exit 1
fi

python3 ./tests_protocol.py
echo "IDZ4 integration tests passed."
