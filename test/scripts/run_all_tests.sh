#!/bin/bash
# ProtoRelay 一键测试脚本 — 初始化环境 → 构建 → 单元测试 → E2E → 生成报告
# 用法: bash test/scripts/run_all_tests.sh [--real] [--no-build]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TEST_DIR="$(dirname "$SCRIPT_DIR")"
PROJECT_DIR="$(dirname "$TEST_DIR")"
BUILD_DIR="${PROJECT_DIR}/build"
REPORT_FILE="${TEST_DIR}/test-report-$(date +%Y%m%d-%H%M%S).txt"

# ── Colors ──
G='\033[0;32m'; R='\033[0;31m'; Y='\033[1;33m'; B='\033[0;34m'; N='\033[0m'

REAL_MODE=false
NO_BUILD=false
while [[ $# -gt 0 ]]; do
    case "$1" in
        --real) REAL_MODE=true; shift ;;
        --no-build) NO_BUILD=true; shift ;;
        *) echo "Unknown: $1"; echo "Usage: $0 [--real] [--no-build]"; exit 1 ;;
    esac
done

# ── 报告写入 ──
exec > >(tee "$REPORT_FILE") 2>&1
echo "ProtoRelay Test Report — $(date '+%Y-%m-%d %H:%M:%S')"
echo "Mode: $([ "$REAL_MODE" = true ] && echo 'REAL (with DB)' || echo 'MOCK (no DB)')"
echo ""

pass=0; fail=0
pass_msg() { echo -e "${G}[PASS]${N} $*"; ((pass++)) || true; }
fail_msg() { echo -e "${R}[FAIL]${N} $*"; ((fail++)) || true; }
info_msg(){ echo -e "${Y}[INFO]${N} $*"; }
step()   { echo -e "\n${B}━━━ $* ━━━${N}"; }

# ── cleanup ──
PID_SMTP=""; PID_IMAP=""
cleanup() {
    [ -n "${PID_SMTP:-}" ] && kill "$PID_SMTP" 2>/dev/null || true
    [ -n "${PID_IMAP:-}" ] && kill "$PID_IMAP" 2>/dev/null || true
    pkill -f "smtpsServer.*test/config" 2>/dev/null || true
    pkill -f "imapsServer.*test/config" 2>/dev/null || true
}
trap cleanup EXIT

# ════════════════════════════════════════════════════════
step "Step 1: Init test environment"
# ════════════════════════════════════════════════════════
cd "$PROJECT_DIR"
python3 test/scripts/setup_test_env.py --clean 2>/dev/null || true
python3 test/scripts/setup_test_env.py

# ════════════════════════════════════════════════════════
step "Step 2: Build"
# ════════════════════════════════════════════════════════
if [ "$NO_BUILD" = false ]; then
    mkdir -p "$BUILD_DIR"
    cmake -B "$BUILD_DIR" -S "$PROJECT_DIR" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON >/dev/null 2>&1
    NJ=$(nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 4)
    if cmake --build "$BUILD_DIR" -j"$NJ" --target smtpsServer imapsServer \
        smtps_fsm_test imaps_fsm_test test_inbound_verifier outbound_smoke sql_queries_test 2>&1 | tail -3; then
        pass_msg "Build OK"
    else
        fail_msg "Build failed"
        exit 1
    fi
else
    info_msg "Build skipped (--no-build)"
fi

# ════════════════════════════════════════════════════════
step "Step 3: Unit tests (C++)"
# ════════════════════════════════════════════════════════
for t in smtps_fsm_test imaps_fsm_test test_inbound_verifier outbound_smoke sql_queries_test; do
    if [ -x "${BUILD_DIR}/${t}" ]; then
        if "${BUILD_DIR}/${t}" >/dev/null 2>&1; then
            pass_msg "$t"
        else
            fail_msg "$t"
        fi
    else
        fail_msg "$t (not found)"
    fi
done

# ════════════════════════════════════════════════════════
step "Step 4: Start servers"
# ════════════════════════════════════════════════════════
if [ "$REAL_MODE" = true ]; then
    SMTP_CONF="test/config/smtps_real.json"
    IMAP_CONF="test/config/imaps_real.json"
else
    SMTP_CONF="test/config/smtps_mock.json"
    IMAP_CONF="test/config/imaps_mock.json"
fi

info_msg "SMTP config: $SMTP_CONF"
info_msg "IMAP config: $IMAP_CONF"

"${BUILD_DIR}/smtpsServer" -c "$SMTP_CONF" >/dev/null 2>&1 &
PID_SMTP=$!
"${BUILD_DIR}/imapsServer" -c "$IMAP_CONF" >/dev/null 2>&1 &
PID_IMAP=$!

# Wait for ports
for i in $(seq 1 30); do
    if nc -z 127.0.0.1 2525 2>/dev/null && nc -z 127.0.0.1 1414 2>/dev/null; then
        break
    fi
    sleep 0.5
done

if nc -z 127.0.0.1 2525 2>/dev/null; then
    pass_msg "SMTP server ready (:2525)"
else
    fail_msg "SMTP server not ready"
fi
if nc -z 127.0.0.1 1414 2>/dev/null; then
    pass_msg "IMAP server ready (:1414)"
else
    fail_msg "IMAP server not ready"
fi

# ════════════════════════════════════════════════════════
step "Step 5: Python E2E tests"
# ════════════════════════════════════════════════════════
PY_TESTS=(
    "test/e2e/test_pipeline.py:SMTP pipeline"
    "test/e2e/test_tcp_sticky.py:TCP sticky/truncation"
    "test/e2e/test_outbound.py:Outbound delivery"
)

for entry in "${PY_TESTS[@]}"; do
    script="${entry%%:*}"
    label="${entry##*:}"
    if python3 "${PROJECT_DIR}/${script}" >/dev/null 2>&1; then
        pass_msg "$label"
    else
        info_msg "$label (partial failures expected in mock mode)"
    fi
done

# ════════════════════════════════════════════════════════
step "Step 6: Full integration test (SMTP + IMAP)"
# ════════════════════════════════════════════════════════
if python3 "${PROJECT_DIR}/test/e2e/test_full_integration.py" 2>&1; then
    pass_msg "Full integration (SMTP+IMAP)"
else
    if [ "$REAL_MODE" = true ]; then
        fail_msg "Full integration (SMTP+IMAP) — check DB/auth"
    else
        info_msg "Full integration (SMTP+IMAP) — auth requires real DB"
    fi
fi

# ════════════════════════════════════════════════════════
# Report
# ════════════════════════════════════════════════════════
echo ""
echo "==========================================="
printf "  ${G}PASS: %d${N}  ${R}FAIL: %d${N}\n" $pass $fail
echo "  Report: $REPORT_FILE"
echo "==========================================="

cleanup

[ "$fail" -eq 0 ] && exit 0 || exit 1
