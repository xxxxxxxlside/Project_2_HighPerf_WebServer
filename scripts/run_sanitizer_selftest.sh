#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD_DIR="${ROOT_DIR}/build_asan_ubsan"
RESULT_DIR="${ROOT_DIR}/results/week3_day4/asan_ubsan"
DURATION_SEC="${DURATION_SEC:-300}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --duration-sec)
            DURATION_SEC="$2"
            shift 2
            ;;
        --build-dir)
            BUILD_DIR="$2"
            shift 2
            ;;
        --result-dir)
            RESULT_DIR="$2"
            shift 2
            ;;
        *)
            echo "Unknown argument: $1" >&2
            exit 1
            ;;
    esac
done

mkdir -p "${RESULT_DIR}"

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=AsanUbsan
cmake --build "${BUILD_DIR}" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

SERVER_STDOUT="${RESULT_DIR}/server.stdout.log"
SERVER_STDERR="${RESULT_DIR}/server.stderr.log"
CLIENT_LOG="${RESULT_DIR}/client.log"
METRICS_LOG="${RESULT_DIR}/metrics.log"

rm -f "${SERVER_STDOUT}" "${SERVER_STDERR}" "${CLIENT_LOG}" "${METRICS_LOG}"

cleanup() {
    if [[ -n "${SERVER_PID:-}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
        kill -TERM "${SERVER_PID}" 2>/dev/null || true
        wait "${SERVER_PID}" || true
    fi
}

trap cleanup EXIT

pushd "${RESULT_DIR}" >/dev/null
ASAN_OPTIONS="detect_leaks=1:halt_on_error=1:abort_on_error=1:strict_string_checks=1:check_initialization_order=1" \
UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1" \
"${BUILD_DIR}/web_server" >"${SERVER_STDOUT}" 2>"${SERVER_STDERR}" &
SERVER_PID=$!
popd >/dev/null

python3 - <<'PY'
import socket
import time

deadline = time.time() + 10.0
while time.time() < deadline:
    try:
        with socket.create_connection(("127.0.0.1", 8080), timeout=0.2):
            raise SystemExit(0)
    except OSError:
        time.sleep(0.05)
raise SystemExit("server did not start listening on 127.0.0.1:8080")
PY

python3 - "${DURATION_SEC}" >"${CLIENT_LOG}" <<'PY'
import socket
import sys
import threading
import time

DURATION_SEC = float(sys.argv[1])
HOST = "127.0.0.1"
PORT = 8080
THREADS = 1
errors = []
stop_event = threading.Event()


def recv_all(sock):
    data = b""
    while b"\r\n\r\n" not in data:
        try:
            chunk = sock.recv(4096)
        except TimeoutError:
            break
        except ConnectionResetError:
            break
        if not chunk:
            break
        data += chunk

    if b"\r\n\r\n" not in data:
        return data

    header_bytes, body = data.split(b"\r\n\r\n", 1)
    content_length = 0
    for line in header_bytes.split(b"\r\n")[1:]:
        if line.lower().startswith(b"content-length:"):
            content_length = int(line.split(b":", 1)[1].strip())
            break

    while len(body) < content_length:
        try:
            chunk = sock.recv(4096)
        except TimeoutError:
            break
        except ConnectionResetError:
            break
        if not chunk:
            break
        body += chunk

    return header_bytes + b"\r\n\r\n" + body[:content_length]


def request(raw: bytes) -> bytes:
    with socket.create_connection((HOST, PORT), timeout=2.0) as sock:
        sock.sendall(raw)
        return recv_all(sock)


def expect_status(name: str, raw: bytes, expected: bytes) -> None:
    response = request(raw)
    if expected not in response:
        raise RuntimeError(f"{name} expected {expected!r}, got {response[:120]!r}")
    print(f"{name}: ok")


def smoke_checks() -> None:
    expect_status(
        "basic_get",
        b"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n",
        b"200 OK",
    )
    expect_status(
        "basic_post",
        b"POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 4\r\n\r\ntest",
        b"200 OK",
    )
    expect_status(
        "missing_content_length",
        b"POST / HTTP/1.1\r\nHost: localhost\r\n\r\n",
        b"411 Length Required",
    )
    expect_status(
        "chunked_reject",
        b"POST / HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n",
        b"501 Not Implemented",
    )
    long_header = b"X-Long: " + (b"a" * 9000)
    expect_status(
        "header_too_large",
        b"GET / HTTP/1.1\r\nHost: localhost\r\n" + long_header + b"\r\n\r\n",
        b"431 Request Header Fields Too Large",
    )
    expect_status(
        "body_too_large",
        b"POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 9000000\r\n\r\n",
        b"413 Payload Too Large",
    )


def worker(worker_id: int, deadline: float) -> None:
    count = 0
    while not stop_event.is_set() and time.time() < deadline:
        try:
            if count % 3 == 0:
                payload = b"POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 16\r\n\r\n0123456789abcdef"
            else:
                payload = b"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n"

            response = request(payload)
            if not response:
                time.sleep(0.02)
                response = request(payload)

            if b"200 OK" not in response:
                raise RuntimeError(
                    f"worker {worker_id} unexpected response {response[:120]!r}"
                )
            count += 1
            time.sleep(0.05)
        except Exception as exc:
            errors.append(str(exc))
            stop_event.set()
            return
    print(f"worker_{worker_id}_requests={count}")


smoke_checks()
deadline = time.time() + DURATION_SEC
threads = [
    threading.Thread(target=worker, args=(i, deadline), daemon=True)
    for i in range(THREADS)
]

for thread in threads:
    thread.start()
for thread in threads:
    thread.join()

if errors:
    raise SystemExit("\n".join(errors))

print("load_phase: ok")
PY

kill -TERM "${SERVER_PID}"
wait "${SERVER_PID}"
trap - EXIT

if [[ ! -f "${METRICS_LOG}" ]]; then
    echo "missing metrics log: ${METRICS_LOG}" >&2
    exit 1
fi

if grep -Eq "AddressSanitizer|LeakSanitizer|runtime error:|UndefinedBehaviorSanitizer" "${SERVER_STDERR}"; then
    echo "sanitizer reported an error, see ${SERVER_STDERR}" >&2
    exit 1
fi

echo "asan/ubsan self-test passed"
echo "result_dir=${RESULT_DIR}"
