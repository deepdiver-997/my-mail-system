#!/bin/bash
# SMTP benchmark launcher — ramp concurrency to find server limits.
#
# Usage:
#   ./bench.sh                         # all modes, conn-pool strategy
#   ./bench.sh --mode mta-relay        # single mode
#   ./bench.sh --mode submission       # STARTTLS+AUTH on port 587
#   ./bench.sh --strategy pipeline     # all modes, pipeline strategy
#   ./bench.sh --mode mta-relay --strategy pipeline --messages 20000
#
# Modes:  mta-relay | submission | smtps | all
# Strategies: conn-pool | per-conn | pipeline | all

set -euo pipefail

# ── defaults ───────────────────────────────────────────────────────────────
HOST="${HOST:-127.0.0.1}"
MESSAGES="${MESSAGES:-10000}"
RAMP_START="${RAMP_START:-50}"
RAMP_END="${RAMP_END:-400}"
RAMP_STEP="${RAMP_STEP:-50}"
TIMEOUT="${TIMEOUT:-15}"

# hardcoded credentials
USER="test2@scut.email"
PASSWORD="test123"

MODE="all"
STRATEGY="conn-pool"
EXTRA_FLAGS=""

# ── parse args ─────────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --mode)
            MODE="$2"; shift 2 ;;
        --strategy)
            STRATEGY="$2"; shift 2 ;;
        --messages)
            MESSAGES="$2"; shift 2 ;;
        --ramp-start)
            RAMP_START="$2"; shift 2 ;;
        --ramp-end)
            RAMP_END="$2"; shift 2 ;;
        --ramp-step)
            RAMP_STEP="$2"; shift 2 ;;
        --host)
            HOST="$2"; shift 2 ;;
        --timeout)
            TIMEOUT="$2"; shift 2 ;;
        --verbose)
            EXTRA_FLAGS="$EXTRA_FLAGS --verbose"; shift ;;
        *)
            echo "Unknown: $1"
            exit 1 ;;
    esac
done

# ── helpers ────────────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "$0")/../scripts" && pwd)"
CL="$SCRIPT_DIR/cl.py"

auth_flags() {
    echo "--user $USER --password $PASSWORD"
}

run_ramp() {
    local label="$1"; shift
    echo ""
    echo "============================================================"
    echo "  $label"
    echo "  messages=$MESSAGES  ramp=$RAMP_START..$RAMP_END/$RAMP_STEP"
    echo "============================================================"
    python3 "$CL" \
        --host "$HOST" \
        --messages "$MESSAGES" \
        --timeout "$TIMEOUT" \
        --ramp \
        --ramp-start "$RAMP_START" \
        --ramp-end "$RAMP_END" \
        --ramp-step "$RAMP_STEP" \
        $EXTRA_FLAGS \
        "$@"
}

run_ramp_for_mode() {
    local mode="$1"
    local strategy="$2"
    local extra=()
    case "$mode" in
        mta-relay)
            run_ramp "MTA-RELAY / $strategy" \
                --mode mta-relay --strategy "$strategy"
            ;;
        submission)
            run_ramp "SUBMISSION / $strategy" \
                --mode submission --strategy "$strategy" $(auth_flags)
            ;;
        smtps)
            run_ramp "SMTPS / $strategy" \
                --mode smtps --strategy "$strategy" $(auth_flags)
            ;;
    esac
}

# ── main ───────────────────────────────────────────────────────────────────
echo "Host:     $HOST"
echo "Messages: $MESSAGES"
echo "Ramp:     $RAMP_START → $RAMP_END (step $RAMP_STEP)"
echo "User:     $USER"
echo ""

modes=()
strategies=()

case "$MODE" in
    all) modes=(mta-relay submission smtps) ;;
    *)   modes=("$MODE") ;;
esac

case "$STRATEGY" in
    all) strategies=(conn-pool per-conn pipeline) ;;
    *)   strategies=("$STRATEGY") ;;
esac

for mode in "${modes[@]}"; do
    for strat in "${strategies[@]}"; do
        # pipeline with auth requires auth flags which are already added
        run_ramp_for_mode "$mode" "$strat"
    done
done

echo ""
echo "All benchmarks complete."
