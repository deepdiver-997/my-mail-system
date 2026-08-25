#!/usr/bin/env bash
# ================================================================
# 覆盖率测量：构建打桩单测 → 运行 → 合并 → 生成报告
#
# 工具链自动探测：
#   clang 路径（macOS 默认）：xcrun llvm-profdata/llvm-cov（与 Apple clang 匹配）
#                             → 回退 PATH → 回退 /opt/homebrew/opt/llvm/bin
#   gcc 路径（Linux CI）    ：lcov + genhtml
#
# 用法：
#   bash test/scripts/coverage.sh            # 本地全流程（clang 或 gcc）
#   bash test/scripts/coverage.sh --ci       # CI 模式：仅 html + 汇总，不写 docs/reports
#
# 产物：
#   build-cov/cov/all.profdata   （clang）合并后的 profile
#   build-cov/html/              HTML 覆盖率详情（浏览器打开 index.html）
#   docs/reports/coverage-<date>.md  汇总报告（本地模式；--ci 跳过）
# ================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

BUILD_DIR="build-cov"
COV_DIR="${BUILD_DIR}/cov"
HTML_DIR="${BUILD_DIR}/html"
REPORT_DIR="docs/reports"
CI_MODE=0
[ "${1:-}" = "--ci" ] && CI_MODE=1

# ---- 工具链探测 --------------------------------------------------
find_llvm() {
    local tool="$1"
    if command -v xcrun >/dev/null 2>&1 && xcrun --find "$tool" >/dev/null 2>&1; then
        echo "$(xcrun --find "$tool")"
    elif command -v "$tool" >/dev/null 2>&1; then
        echo "$(command -v "$tool")"
    elif [ -x "/opt/homebrew/opt/llvm/bin/$tool" ]; then
        echo "/opt/homebrew/opt/llvm/bin/$tool"
    fi
}
LLVM_PROFDATA="$(find_llvm llvm-profdata || true)"
LLVM_COV="$(find_llvm llvm-cov || true)"

MODE="clang"
if [ -z "$LLVM_PROFDATA" ] || [ -z "$LLVM_COV" ]; then
    if command -v lcov >/dev/null 2>&1 && command -v genhtml >/dev/null 2>&1; then
        MODE="gcc"
    else
        echo "ERROR: 缺少覆盖率工具。" >&2
        echo "  macOS: Xcode 自带（xcrun llvm-profdata / llvm-cov）" >&2
        echo "  Linux: sudo apt install lcov" >&2
        exit 1
    fi
fi
echo "==> 覆盖率模式: ${MODE}"

# ---- 构建打桩单测 -------------------------------------------------
cmake -B "$BUILD_DIR" -DENABLE_COVERAGE=ON -DCMAKE_BUILD_TYPE=Debug >/dev/null
NPROC="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
cmake --build "$BUILD_DIR" --target unit_tests -j"$NPROC"

rm -rf "$COV_DIR" "$HTML_DIR"
mkdir -p "$COV_DIR" "$HTML_DIR"

# 参与统计的单测二进制（与 CMakeLists 的 unit_tests DEPENDS 清单保持一致，
# 新增单测后这里也要补上 —— 漏了只是报告少了该二进制的数据，不影响 CI 红绿）
TESTS=(smtps_fsm_test smtps_fsm_concurrency_test imaps_fsm_test test_inbound_verifier
       outbound_smoke sql_queries_test mime_parser_test mail_body_writer_test
       buffered_upload_stream_test io_error_test async_storage_provider_test
       fsm_base_test fast_fsm_base_test intrusion_detector_test server_config_test
       mapped_file_test thread_pool_test server_base_test session_base_test)

if [ "$MODE" = "clang" ]; then
    # ---- 运行 + 合并（clang 源级覆盖率）----
    # 每个测试进程独立 .profraw（%p = pid），串行 ctest 天然无冲突
    (cd "$BUILD_DIR" && LLVM_PROFILE_FILE="$PWD/cov/%p.profraw" ctest --output-on-failure)
    "$LLVM_PROFDATA" merge -sparse "$COV_DIR"/*.profraw -o "$COV_DIR/all.profdata"

    OBJECTS=()
    for t in "${TESTS[@]}"; do
        [ -x "$BUILD_DIR/$t" ] && OBJECTS+=(-object "$BUILD_DIR/$t")
    done

    # 过滤：只看项目 include/ + src/（排除 test/、generated/、系统头）
    IGNORE='.*(test/|generated/|/usr/|/opt/|\.venv/|build-cov/).*'

    echo
    echo "===== llvm-cov 汇总（行/区域/函数）====="
    "$LLVM_COV" report -instr-profile="$COV_DIR/all.profdata" \
        -ignore-filename-regex="$IGNORE" "${OBJECTS[@]}"

    echo
    echo "===== 生成 HTML（build-cov/html）====="
    "$LLVM_COV" show -format=html -instr-profile="$COV_DIR/all.profdata" \
        -ignore-filename-regex="$IGNORE" -output-dir="$HTML_DIR" "${OBJECTS[@]}"

    # 导出 JSON 供报告生成器聚合模块
    "$LLVM_COV" export -summary-only -instr-profile="$COV_DIR/all.profdata" \
        -ignore-filename-regex="$IGNORE" "${OBJECTS[@]}" > "$COV_DIR/export.json"

    if [ "$CI_MODE" = "1" ]; then
        echo "CI 模式，跳过 docs/reports 写入"
    else
        python3 test/scripts/coverage_report.py llvm "$COV_DIR/export.json"
    fi
else
    # ---- 运行 + lcov（gcc 路径，Linux CI）----
    (cd "$BUILD_DIR" && ctest --output-on-failure)
    lcov --capture --directory "$BUILD_DIR" --output-file "$COV_DIR/base.info" --quiet
    lcov --remove "$COV_DIR/base.info" \
        '/usr/*' '*/test/*' '*/generated/*' '*/.venv/*' '*/build-cov/*' \
        --output-file "$COV_DIR/filtered.info" --quiet
    lcov --summary "$COV_DIR/filtered.info"
    genhtml "$COV_DIR/filtered.info" -o "$HTML_DIR" --quiet

    if [ "$CI_MODE" = "1" ]; then
        echo "CI 模式，跳过 docs/reports 写入"
    else
        python3 test/scripts/coverage_report.py lcov "$COV_DIR/filtered.info"
    fi
fi

echo
echo "==> HTML 报告: ${HTML_DIR}/index.html"
