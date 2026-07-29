#!/bin/bash
# Run all 4 SMTP delivery path benchmarks with appropriate message counts.
# Per-conn tests limited to 5000 msgs due to localhost ephemeral port limit (~16384).
set -euo pipefail
PROJECT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="$PROJECT_DIR/build/smtp_client"
TW_WAIT=35

cd "$PROJECT_DIR"

do_ramp() {
    local label="$1"; shift
    local msgs="$1"; shift
    local threads="$1"; shift
    local flags=("$@")
    local best_t=0 best_r=0
    echo ""
    echo "============================================================"
    echo "  $label  (msgs=$msgs)"
    echo "============================================================"
    for t in $threads; do
        local out=$("$BIN" "${flags[@]}" --t "$t" --msgs "$msgs" 2>&1)
        echo "  $out"
        local rate=$(echo "$out" | grep -oE 'rate=[0-9]+' | grep -oE '[0-9]+')
        if [ -n "$rate" ] && [ "$rate" -gt "$best_r" ] 2>/dev/null; then
            best_r=$rate; best_t=$t
        fi
    done
    echo "  >> Peak: $best_r msg/s @ t=$best_t"
}

wait_drain() {
    echo -n "  Draining TIME_WAIT..."
    while [ $(netstat -an | grep -c TIME_WAIT 2>/dev/null || echo 0) -gt 50 ]; do
        sleep 3
    done
    sleep "$TW_WAIT"
    echo " done ($(netstat -an | grep -c TIME_WAIT 2>/dev/null) remaining)"
}

# ── Benchmark 1: sequential + per-conn ──
do_ramp "sequential + per-conn" 5000 "1 2 4 8 16" --seq
echo "  (waiting for TIME_WAIT drain, 35s...)"
wait_drain

# ── Benchmark 2: sequential + reuse ──
bash test/scripts/cleanup.sh
do_ramp "sequential + reuse (MTA reuse)" 5000 "1 2 4 8 16 32" --seq --reuse

# ── Benchmark 3: pipeline + per-conn ──
bash test/scripts/cleanup.sh
do_ramp "pipeline + per-conn" 5000 "1 2 4 8" --pipe
echo "  (waiting for TIME_WAIT drain, 35s...)"
wait_drain

# ── Benchmark 4: pipeline + reuse (max throughput) ──
bash test/scripts/cleanup.sh
do_ramp "pipeline + reuse (max throughput)" 50000 "1 2 4 8 16 32 64" --pipe --reuse

# final cleanup
bash test/scripts/cleanup.sh
echo ""
echo "All benchmarks complete."
