#!/usr/bin/env bash
# ──────────────────────────────────────────────────────────────────────────────
# run_benchmark.sh — runs all 4 scenarios automatically
#
# Usage:
#   bash scripts/run_benchmark.sh [--count N]
# ──────────────────────────────────────────────────────────────────────────────

set -euo pipefail

COUNT="${1:-100000}"
PORT=9999
RESULTS_DIR="results"
mkdir -p "$RESULTS_DIR"

echo ""
echo "  TCP Latency Lab — Full Benchmark"
echo "  Messages per scenario: $COUNT"
echo ""

# ── Verify binaries exist ─────────────────────────────────────────────────────
if [[ ! -f ./server ]] || [[ ! -f ./client ]]; then
    echo "  [ERROR] Binaries not found. Run: make"
    exit 1
fi

# ── Helper: start server, wait for it, run client, kill server ───────────────
run_scenario() {
    local mode="$1"
    local server_opts="$2"
    local scenario_name="$3"

    echo "  ─────────────────────────────────────────"
    echo "  Scenario $mode: $scenario_name"
    echo "  Server opts: $server_opts"
    echo ""

    # Start server in background
    ./server $server_opts --port $PORT &
    SERVER_PID=$!

    # Wait for server to be ready
    sleep 0.3

    # Run client for just this scenario
    ./client --port $PORT --count $COUNT --mode $mode \
             --warmup 5000 2>&1 | tee "$RESULTS_DIR/scenario_${mode}.txt"

    # Stop server
    kill $SERVER_PID 2>/dev/null || true
    wait $SERVER_PID 2>/dev/null || true
    sleep 0.2
}

# ── Scenario 0: Baseline (Nagle ON) ──────────────────────────────────────────
run_scenario 0 "" "Baseline (Nagle ON)"

# ── Scenario 1: TCP_NODELAY ───────────────────────────────────────────────────
run_scenario 1 "--nodelay" "TCP_NODELAY"

# ── Scenario 2: NODELAY + CPU pin ────────────────────────────────────────────
run_scenario 2 "--nodelay --affinity 0" "NODELAY + CPU Pin"

# ── Scenario 3: All optimizations ────────────────────────────────────────────
run_scenario 3 "--nodelay --quickack --affinity 0" "All Optimizations"

echo ""
echo "  ✓ All scenarios complete."
echo "  Results in: $RESULTS_DIR/"
echo ""
echo "  CSV data for README table:"
cat "$RESULTS_DIR/benchmark.csv" 2>/dev/null || echo "  (run client without --mode to generate CSV)"
echo ""
