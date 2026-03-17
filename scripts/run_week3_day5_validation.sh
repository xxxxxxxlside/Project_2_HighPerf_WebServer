#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD_DIR="${ROOT_DIR}/build"
RUN_DIR="${RUN_DIR:-$(mktemp -d /tmp/project2_week3_day5.XXXXXX)}"
HOST="${HOST:-127.0.0.1}"
PORT="${PORT:-8080}"
THREADS="${THREADS:-8}"
CONNS="${CONNS:-100}"
WARMUP_SEC="${WARMUP_SEC:-5}"
MEASURE_SEC="${MEASURE_SEC:-30}"
TOTAL_SEC=$((WARMUP_SEC + MEASURE_SEC))

SERVER_STDOUT="${RUN_DIR}/server.stdout.log"
SERVER_STDERR="${RUN_DIR}/server.stderr.log"
METRICS_LOG="${RUN_DIR}/metrics.log"
LOAD_LOG="${RUN_DIR}/load.log"

mkdir -p "${RUN_DIR}"

cleanup() {
    if [[ -n "${LOAD_PID:-}" ]] && kill -0 "${LOAD_PID}" 2>/dev/null; then
        kill -TERM "${LOAD_PID}" 2>/dev/null || true
        wait "${LOAD_PID}" || true
    fi
    if [[ -n "${SERVER_PID:-}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
        kill -TERM "${SERVER_PID}" 2>/dev/null || true
        wait "${SERVER_PID}" || true
    fi
}

trap cleanup EXIT

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}"
cmake --build "${BUILD_DIR}" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

pushd "${RUN_DIR}" >/dev/null
"${BUILD_DIR}/web_server" >"${SERVER_STDOUT}" 2>"${SERVER_STDERR}" &
SERVER_PID=$!
popd >/dev/null

python3 - "${HOST}" "${PORT}" <<'PY'
import socket
import sys
import time

host = sys.argv[1]
port = int(sys.argv[2])
deadline = time.time() + 10.0
while time.time() < deadline:
    try:
        with socket.create_connection((host, port), timeout=0.2):
            raise SystemExit(0)
    except OSError:
        time.sleep(0.05)
raise SystemExit(f"server did not start listening on {host}:{port}")
PY

if command -v wrk >/dev/null 2>&1; then
    WRK_CMD=(wrk -t"${THREADS}" -c"${CONNS}" -d"${TOTAL_SEC}s" --latency "http://${HOST}:${PORT}/")
    printf '%s\n' "${WRK_CMD[*]}" | tee "${LOAD_LOG}"
    "${WRK_CMD[@]}" >>"${LOAD_LOG}" 2>&1 &
    LOAD_PID=$!
else
    echo "wrk not found, using built-in keep-alive load generator" | tee "${LOAD_LOG}"
    python3 - "${HOST}" "${PORT}" "${THREADS}" "${CONNS}" "${TOTAL_SEC}" "${WARMUP_SEC}" >>"${LOAD_LOG}" 2>&1 <<'PY' &
import errno
import socket
import sys
import threading
import time

HOST = sys.argv[1]
PORT = int(sys.argv[2])
THREADS = int(sys.argv[3])
CONNS = int(sys.argv[4])
DURATION_SEC = float(sys.argv[5])
WARMUP_SEC = float(sys.argv[6])
REQUEST = (
    b"GET / HTTP/1.1\r\n"
    b"Host: localhost\r\n"
    b"Connection: keep-alive\r\n"
    b"\r\n"
)

errors = []
total_requests = 0
lock = threading.Lock()


def read_response(sock: socket.socket) -> bytes:
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = sock.recv(4096)
        if not chunk:
            raise RuntimeError("connection closed before header complete")
        data += chunk

    header_bytes, body = data.split(b"\r\n\r\n", 1)
    content_length = 0
    for line in header_bytes.split(b"\r\n")[1:]:
        if line.lower().startswith(b"content-length:"):
            content_length = int(line.split(b":", 1)[1].strip())
            break

    while len(body) < content_length:
        chunk = sock.recv(4096)
        if not chunk:
            raise RuntimeError("connection closed before body complete")
        body += chunk

    return header_bytes + b"\r\n\r\n" + body[:content_length]


def open_sockets(connection_count: int):
    sockets = []
    for _ in range(connection_count):
        sock = socket.create_connection((HOST, PORT), timeout=2.0)
        sock.settimeout(2.0)
        sockets.append(sock)
    return sockets


def worker(connection_count: int, deadline: float, warmup_deadline: float) -> None:
    global total_requests
    initial_count = max(1, connection_count * 4 // 5)
    deferred_count = connection_count - initial_count
    sockets = []
    local_count = 0
    try:
        sockets.extend(open_sockets(initial_count))

        while time.time() < deadline:
            if deferred_count > 0 and time.time() >= warmup_deadline + 0.5:
                sockets.extend(open_sockets(deferred_count))
                deferred_count = 0
            for sock in sockets:
                sock.sendall(REQUEST)
                response = read_response(sock)
                if b"200 OK" not in response:
                    raise RuntimeError(f"unexpected response: {response[:120]!r}")
                if b"Connection: close" in response:
                    raise RuntimeError("server closed keep-alive response during day5 validation")
                local_count += 1
                if time.time() >= deadline:
                    break
    except (OSError, RuntimeError) as exc:
        errors.append(str(exc))
    finally:
        for sock in sockets:
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError as exc:
                if exc.errno not in (errno.ENOTCONN, errno.EBADF):
                    pass
            try:
                sock.close()
            except OSError:
                pass
        with lock:
            total_requests += local_count


deadline = time.time() + DURATION_SEC
warmup_deadline = time.time() + WARMUP_SEC
base = CONNS // THREADS
extra = CONNS % THREADS
threads = []
for index in range(THREADS):
    count = base + (1 if index < extra else 0)
    if count == 0:
        continue
    thread = threading.Thread(target=worker, args=(count, deadline, warmup_deadline), daemon=True)
    thread.start()
    threads.append(thread)

for thread in threads:
    thread.join()

if errors:
    raise SystemExit("\n".join(errors))

print(f"built_in_keepalive_requests={total_requests}")
PY
    LOAD_PID=$!
fi

sleep "${WARMUP_SEC}"

BASELINE_JSON=$(python3 - "${METRICS_LOG}" <<'PY'
import json
import sys
from pathlib import Path

path = Path(sys.argv[1])
if not path.exists():
    raise SystemExit("metrics log missing during warmup")

lines = [line.strip() for line in path.read_text().splitlines() if line.strip()]
if not lines:
    raise SystemExit("metrics log empty during warmup")

values = {}
for part in lines[-1].split():
    key, value = part.split("=", 1)
    values[key] = int(value)

print(json.dumps(values))
PY
)

wait "${LOAD_PID}"
unset LOAD_PID

FINAL_JSON=$(python3 - "${METRICS_LOG}" <<'PY'
import json
import sys
from pathlib import Path

path = Path(sys.argv[1])
if not path.exists():
    raise SystemExit("metrics log missing after load")

lines = [line.strip() for line in path.read_text().splitlines() if line.strip()]
if not lines:
    raise SystemExit("metrics log empty after load")

values = {}
for part in lines[-1].split():
    key, value = part.split("=", 1)
    values[key] = int(value)

print(json.dumps(values))
PY
)

kill -TERM "${SERVER_PID}"
wait "${SERVER_PID}"
unset SERVER_PID
trap - EXIT

python3 - "${BASELINE_JSON}" "${FINAL_JSON}" "${THREADS}" "${CONNS}" "${WARMUP_SEC}" "${MEASURE_SEC}" <<'PY'
import json
import sys

baseline = json.loads(sys.argv[1])
final = json.loads(sys.argv[2])
threads = int(sys.argv[3])
connections = int(sys.argv[4])
warmup = int(sys.argv[5])
measure = int(sys.argv[6])

accept_delta = final["accept_total"] - baseline["accept_total"]
request_delta = final["requests_total"] - baseline["requests_total"]
reuse_ratio = (request_delta / accept_delta) if accept_delta > 0 else 0.0

print(f"validation_window={measure}s warmup={warmup}s threads={threads} conns={connections}")
print(f"accept_delta={accept_delta}")
print(f"request_delta={request_delta}")
print(f"reuse_ratio={reuse_ratio:.2f}")

if accept_delta <= 0:
    raise SystemExit("invalid run: accept_delta <= 0")
if reuse_ratio <= 10.0:
    raise SystemExit("day5 validation failed: reuse_ratio <= 10")
PY

echo "run_dir=${RUN_DIR}"
