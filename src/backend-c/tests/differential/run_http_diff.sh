#!/bin/sh
# HTTP API contract differential test (Phase 6, task #37): runs the real
# Go daemon and magitrickled-c against byte-identical scratch configs,
# drives each through the SAME fixed sequence of HTTP/Unix-socket
# requests (http_contract/contract.py), and diffs the two resulting
# traces. Every mutating request in the sequence supplies its own
# explicit IDs, so a passing run means byte-identical (not just
# structurally-equivalent) status codes and JSON bodies across both
# backends for every step -- see http_contract/contract.py's docstring.
#
# Requires root (same as the "config" suite in run_diff.sh: real
# /var/lib/magitrickle/config.yaml, real iptables). Temporarily
# overwrites /var/lib/magitrickle/config.yaml (Go's config path is
# hard-coded, no CLI override) -- any pre-existing file there is backed
# up and restored on exit. Also requires python3 (stdlib only).
set -eu
DIR="$(cd "$(dirname "$0")" && pwd)"
BACKEND_C_DIR="$(cd "$DIR/../.." && pwd)"
BACKEND_GO_DIR="$(cd "$BACKEND_C_DIR/../backend" && pwd)"
OUT="$DIR/out"
mkdir -p "$OUT"

REAL_CONFIG=/var/lib/magitrickle/config.yaml
REAL_CONFIG_BACKUP="$OUT/http_real_config.backup"
SCRATCH_CONFIG="$OUT/http_scratch_config.yaml"
PORT=18095
DNS_PORT=13595
SOCK=/var/run/magitrickle.sock
PIDFILE=/var/run/magitrickle.pid

GO_PID=
C_PID=
cleanup() {
    [ -n "$GO_PID" ] && kill "$GO_PID" 2>/dev/null || true
    [ -n "$C_PID" ] && kill "$C_PID" 2>/dev/null || true
    if [ -f "$REAL_CONFIG_BACKUP" ]; then
        cp "$REAL_CONFIG_BACKUP" "$REAL_CONFIG"
    else
        rm -f "$REAL_CONFIG"
    fi
    rm -f "$SOCK" "$PIDFILE"
}
trap cleanup EXIT

if [ -f "$REAL_CONFIG" ]; then
    cp "$REAL_CONFIG" "$REAL_CONFIG_BACKUP"
else
    rm -f "$REAL_CONFIG_BACKUP"
fi

cat > "$SCRATCH_CONFIG" <<EOF
configVersion: 0.7.0
app:
  httpWeb:
    enabled: true
    auth:
      enabled: false
    host:
      address: "127.0.0.1"
      port: $PORT
    skin: default
  dnsProxy:
    host:
      address: "127.0.0.1"
      port: $DNS_PORT
    upstream:
      address: 127.0.0.1
      port: 53
    disableRemap53: true
    disableFakePTR: false
    disableDropAAAA: false
    maxIdleConns: 10
    maxConcurrent: 100
    timeout: 5s
  netfilter:
    iptables:
      chainPrefix: MT_
    ipset:
      tablePrefix: mt_
      additionalTTL: 1h0m0s
    disableIPv4: false
    disableIPv6: false
    startMarkTableIndex: 1298229097
  link: []
  showAllInterfaces: true
  logLevel: error
groups: []
subscriptions: []
EOF

echo "== building Go daemon"
( cd "$BACKEND_GO_DIR" && go build -o "$OUT/magitrickled-go" ./cmd/magitrickled )

echo "== building C daemon"
( cd "$BACKEND_C_DIR" && make build/host/magitrickled-c >/dev/null )
C_BIN="$BACKEND_C_DIR/build/host/magitrickled-c"

wait_for_port() {
    i=0
    while [ "$i" -lt 50 ]; do
        if curl -s -o /dev/null "http://127.0.0.1:$PORT/api/v1/auth"; then return 0; fi
        i=$((i + 1))
        sleep 0.1
    done
    return 1
}

echo "== running contract against Go daemon"
mkdir -p /var/lib/magitrickle
cp "$SCRATCH_CONFIG" "$REAL_CONFIG"
rm -f "$SOCK" "$PIDFILE"
"$OUT/magitrickled-go" > "$OUT/go_daemon.log" 2>&1 &
GO_PID=$!
if ! wait_for_port; then
    echo "Go daemon failed to start:"; cat "$OUT/go_daemon.log"; exit 1
fi
python3 "$DIR/http_contract/contract.py" 127.0.0.1 "$PORT" "$SOCK" > "$OUT/go.trace" 2> "$OUT/go_contract.err"
CONTRACT_STATUS=$?
kill "$GO_PID" 2>/dev/null || true
wait "$GO_PID" 2>/dev/null || true
GO_PID=
if [ "$CONTRACT_STATUS" -ne 0 ]; then
    echo "contract run against Go failed:"; cat "$OUT/go_contract.err"; exit 1
fi
rm -f "$SOCK" "$PIDFILE"

echo "== running contract against C daemon"
"$C_BIN" --config "$SCRATCH_CONFIG" > "$OUT/c_daemon.log" 2>&1 &
C_PID=$!
if ! wait_for_port; then
    echo "C daemon failed to start:"; cat "$OUT/c_daemon.log"; exit 1
fi
python3 "$DIR/http_contract/contract.py" 127.0.0.1 "$PORT" "$SOCK" > "$OUT/c.trace" 2> "$OUT/c_contract.err"
CONTRACT_STATUS=$?
kill "$C_PID" 2>/dev/null || true
wait "$C_PID" 2>/dev/null || true
C_PID=
if [ "$CONTRACT_STATUS" -ne 0 ]; then
    echo "contract run against C failed:"; cat "$OUT/c_contract.err"; exit 1
fi

echo "== diffing traces"
if diff -u "$OUT/go.trace" "$OUT/c.trace" > "$OUT/http_contract.diff"; then
    echo "   HTTP contract: OK ($(grep -c '^STEP' "$OUT/go.trace") steps)"
else
    echo "   HTTP CONTRACT DIVERGENCE:"
    cat "$OUT/http_contract.diff"
    exit 1
fi
