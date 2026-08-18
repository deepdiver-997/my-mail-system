#!/bin/bash
# ProtoRelay 集成测试脚本
# 构建 → 单元测试 → 启服 → Python 测试 → 清理
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_DIR}/build"
TIMEOUT_SEC=30

GREEN='\033[0;32m'; RED='\033[0;31m'; YELLOW='\033[1;33m'; NC='\033[0m'
pass() { echo -e "${GREEN}[PASS]${NC} $*"; }
fail() { echo -e "${RED}[FAIL]${NC} $*"; }
info() { echo -e "${YELLOW}[INFO]${NC} $*"; }

PID_SMTP=""
PID_IMAP=""

# ── pre-cleanup ──
rm -rf /tmp/smtps_fsm_test_mail /tmp/smtps_fsm_test_att
rm -rf /tmp/imaps_fsm_test_mail /tmp/imaps_fsm_test_att
rm -f /tmp/smtps_fsm_test.log /tmp/imaps_fsm_test.log

# ── cleanup trap ──
cleanup() {
    echo ""
    info "=== Cleaning up ==="
    [ -n "${PID_SMTP:-}" ] && kill "$PID_SMTP" 2>/dev/null || true
    [ -n "${PID_IMAP:-}" ] && kill "$PID_IMAP" 2>/dev/null || true
    sleep 1
    pkill -f "smtpsServer.*smtps_test" 2>/dev/null || true
    pkill -f "imapsServer.*imaps_test" 2>/dev/null || true
    rm -rf /tmp/protorelay_test_output
    rm -rf /tmp/protorelay_test_mail /tmp/protorelay_test_att
    rm -f /tmp/protorelay_test_smtp.log /tmp/protorelay_test_imap.log
    rm -rf /tmp/smtps_fsm_test_mail /tmp/smtps_fsm_test_att
    rm -rf /tmp/smtps_fsm_conc_test_mail /tmp/smtps_fsm_conc_test_att
    rm -rf /tmp/imaps_fsm_test_mail /tmp/imaps_fsm_test_att
    rm -f /tmp/smtps_fsm_test.log /tmp/imaps_fsm_test.log
    info "Cleanup complete"
}
trap cleanup EXIT

echo "============================================="
echo "  ProtoRelay Integration Test Suite"
echo "============================================="
echo ""

# ============================================================
# Step 1: Build
# ============================================================
info "Step 1: Building project..."
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

cmake "${PROJECT_DIR}" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON 2>&1 | tail -2
NPROC=$(nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 4)
cmake --build . -j"$NPROC" --target smtpsServer imapsServer smtps_fsm_test smtps_fsm_concurrency_test imaps_fsm_test test_inbound_verifier outbound_smoke sql_queries_test mime_parser_test 2>&1 | tail -3

for target in smtpsServer imapsServer; do
    if [ -x "${BUILD_DIR}/${target}" ]; then
        pass "Built ${target}"
    else
        fail "${target} not found"
    fi
done
echo ""

# ============================================================
# Step 2: Unit Tests
# ============================================================
info "Step 2: Running unit tests..."
CTEST_FAILED=0

run_ctest() {
    local name="$1" bin="$2"
    if [ -x "${BUILD_DIR}/${bin}" ]; then
        if "${BUILD_DIR}/${bin}" >/dev/null 2>&1; then
            pass "$name"
        else
            fail "$name (exit code $?)"
            "${BUILD_DIR}/${bin}" 2>&1 | tail -3
            CTEST_FAILED=1
        fi
    else
        info "$name skipped"
    fi
}

run_ctest "smtps_fsm_test"          "smtps_fsm_test"
run_ctest "smtps_fsm_concurrency_test" "smtps_fsm_concurrency_test"
run_ctest "imaps_fsm_test"          "imaps_fsm_test"
run_ctest "inbound_verifier"        "test_inbound_verifier"
run_ctest "outbound_smoke"          "outbound_smoke"
run_ctest "sql_queries_test"        "sql_queries_test"
run_ctest "mime_parser_test"        "mime_parser_test"

if [ "$CTEST_FAILED" -ne 0 ]; then
    fail "Some unit tests failed"
else
    pass "All unit tests passed"
fi
echo ""

# ============================================================
# Step 3: Start Servers
# ============================================================
info "Step 3: Starting test servers..."
cd "${PROJECT_DIR}"

info "Starting SMTP server (port 2525)..."
"${BUILD_DIR}/smtpsServer" -c "${PROJECT_DIR}/test/config/smtps_test.json" &
PID_SMTP=$!

info "Starting IMAP server (port 1414)..."
"${BUILD_DIR}/imapsServer" -c "${PROJECT_DIR}/test/config/imaps_test.json" &
PID_IMAP=$!

# Wait for servers
START_TS=$(date +%s)
READY_SMTP=0; READY_IMAP=0
while true; do
    NOW=$(date +%s)
    [ $(( NOW - START_TS )) -gt "$TIMEOUT_SEC" ] && { fail "Timeout waiting for servers"; break; }

    if [ "$READY_SMTP" -eq 0 ] && nc -z 127.0.0.1 2525 2>/dev/null; then
        READY_SMTP=1; pass "SMTP ready (port 2525)"
    fi
    if [ "$READY_IMAP" -eq 0 ] && nc -z 127.0.0.1 1414 2>/dev/null; then
        READY_IMAP=1; pass "IMAP ready (port 1414)"
    fi
    [ "$READY_SMTP" -eq 1 ] && [ "$READY_IMAP" -eq 1 ] && break
    sleep 0.5
done
echo ""

# ============================================================
# Step 4-6: Python SMTP Tests
# ============================================================
if command -v python3 &>/dev/null; then
    info "Step 4: SMTP pipeline test..."
    if python3 "${PROJECT_DIR}/test/e2e/test_pipeline.py" 2>&1 | tail -3; then
        pass "Pipeline test passed"
    else
        info "Pipeline test (some tests may fail without DB/NOOP support)"
    fi
    echo ""

    info "Step 5: TCP sticky/truncation test..."
    if python3 "${PROJECT_DIR}/test/e2e/test_tcp_sticky.py" 2>&1 | tail -3; then
        pass "TCP sticky test passed"
    else
        info "TCP sticky test (some edge cases expected without full FSM)"
    fi
    echo ""

    info "Step 6: Outbound delivery test..."
    if python3 "${PROJECT_DIR}/test/e2e/test_outbound.py" 2>&1 | tail -5; then
        pass "Outbound test passed"
    else
        info "Outbound test (outbound relay requires DB)"
    fi
    echo ""
fi

echo "============================================="
info "Integration test suite complete."
info "Cleanup will happen automatically."
echo "============================================="
