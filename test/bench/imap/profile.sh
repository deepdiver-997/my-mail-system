#!/usr/bin/env bash
# IMAP 读路径 profiling 闭环 —— 高负载下用采样找热点，输出 top-N 热点表。
#
# 流程: 起 release 服务 → 后台压测（imap_client）→ 采样服务 PID N 秒
#       → 折叠成热点 top-N 表（含压测吞吐上下文）→ 追加进 bench-report.md。
#
# 平台（脚本自动选择）:
#   macOS  `sample`（内置，无需安装）
#   Linux  `perf record -F 99 -g -p` + `perf report --stdio`（需安装 linux-tools，
#          且 -p 采样他进程通常要 root/sudo 或调低 perf_event_paranoid）
#
# 用法:
#   ./profile.sh                                      # 默认 release 服务 + 16 并发 + 10s 采样
#   ./profile.sh --sample-secs 15                     # 拉长采样
#   ./profile.sh --load-args "--t 8 --conns 2 --rounds 800"
#   ./profile.sh --server build-release/imapsServer --config /tmp/imap_bench_config.json
#   ./profile.sh --topn 30                            # 热点表行数
#
# 注意: 必须 profile RELEASE 构建——Debug 的热点全是误导（无内联/断言/未优化）。
#       脚本找不到 build-release/imapsServer 时会回退 Debug 并告警。

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"   # imap/ → bench/ → test/ → 根
BENCH_DIR="$ROOT/test/bench"
REPORT="$BENCH_DIR/imap/REPORT.md"

# ── 默认 ───────────────────────────────────────────────────────────────────
SERVER="$ROOT/build-release/imapsServer"
CONFIG="${CONFIG:-/tmp/imap_bench_config.json}"
# 默认负载要跑得比采样窗口久——否则采样窗口后半段全是空闲帧（psynch/kevent），
# 找不到应用热点。16 conns × 2000 rounds ≈ 16s，覆盖 10s 采样。
LOAD_ARGS="${LOAD_ARGS:---t 16 --conns 4 --rounds 2000}"
SAMPLE_SECS=10
TOPN=20

while [[ $# -gt 0 ]]; do
    case "$1" in
        --server)  SERVER="$2"; shift 2 ;;
        --config)  CONFIG="$2"; shift 2 ;;
        --load-args) LOAD_ARGS="$2"; shift 2 ;;
        --sample-secs) SAMPLE_SECS="$2"; shift 2 ;;
        --topn)    TOPN="$2"; shift 2 ;;
        *) echo "unknown arg $1" >&2; exit 1 ;;
    esac
done

# ── 1. 服务二进制 ──────────────────────────────────────────────────────────
if [[ ! -x "$SERVER" ]]; then
    if [[ -x "$ROOT/build/imapsServer" ]]; then
        SERVER="$ROOT/build/imapsServer"
        echo "[warn] release binary not found — falling back to Debug $SERVER"
        echo "[warn] Debug 构建的热点不可靠，建议: cmake --build build-release --target imapsServer -j 4" >&2
    else
        echo "[error] no server binary. Build with:" >&2
        echo "  cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release && cmake --build build-release --target imapsServer -j 4" >&2
        exit 1
    fi
fi
CLIENT="$ROOT/build/imap_client"
[[ -x "$CLIENT" ]] || { echo "[error] imap_client not built: cmake --build build --target imap_client -j 4" >&2; exit 1; }
[[ -f "$CONFIG" ]] || { echo "[error] config not found: $CONFIG" >&2; exit 1; }

STAMP="$(date '+%Y-%m-%d %H:%M')"
UNAME="$(uname -s)"
STACKS="/tmp/profile_stacks_$$.txt"
LOAD_OUT="/tmp/profile_load_$$.out"
SRV_PID=""; LOAD_PID=""

cleanup() { [[ -n "$SRV_PID" ]] && kill "$SRV_PID" 2>/dev/null || true; [[ -n "$LOAD_PID" ]] && kill "$LOAD_PID" 2>/dev/null || true; }
trap cleanup EXIT

# ── 2. 起服务 ──────────────────────────────────────────────────────────────
echo "[profile] server: $SERVER"
"$SERVER" -c "$CONFIG" > /tmp/profile_server.log 2>&1 &
SRV_PID=$!
sleep 2
kill -0 "$SRV_PID" 2>/dev/null || { echo "[error] server failed to start (see /tmp/profile_server.log)" >&2; exit 1; }

# ── 3. 起压测（后台）───────────────────────────────────────────────────────
echo "[profile] load: imap_client $LOAD_ARGS"
"$CLIENT" $LOAD_ARGS > "$LOAD_OUT" 2>&1 &
LOAD_PID=$!
sleep 2   # 让负载爬坡

# ── 4. 采样服务 PID ────────────────────────────────────────────────────────
echo "[profile] sampling server PID $SRV_PID for ${SAMPLE_SECS}s ..."
if [[ "$UNAME" == "Darwin" ]]; then
    sample "$SRV_PID" "$SAMPLE_SECS" > "$STACKS" 2>&1 \
        || { echo "[error] sample failed" >&2; exit 1; }
elif [[ "$UNAME" == "Linux" ]]; then
    PERF_DATA="/tmp/perf_profile_$$.data"
    perf record -F 99 -g -p "$SRV_PID" -o "$PERF_DATA" -- sleep "$SAMPLE_SECS" >/dev/null 2>&1 \
        || { echo "[error] perf record failed (need root? sudo ./profile.sh)" >&2; exit 1; }
    perf report -i "$PERF_DATA" --stdio --sort symbol --no-children > "$STACKS" 2>&1 || true
else
    echo "[error] unsupported platform $UNAME" >&2; exit 1
fi

# ── 5. 等压测收尾，取吞吐 ─────────────────────────────────────────────────
wait "$LOAD_PID" 2>/dev/null || true
LOAD_PID=""
THROUGHPUT="$(grep -E "throughput=" "$LOAD_OUT" | tail -1 || true)"

# ── 6. 折叠成 top-N 热点表 ─────────────────────────────────────────────────
if [[ "$UNAME" == "Darwin" ]]; then
    # "Sort by top of stack, same collapsed" 段:  函数  (in 二进制)  count
    TOP_TABLE="$(perl -ne '
        BEGIN { $n = 0 }
        if (/Sort by top of stack, same collapsed/) { $in = 1; next }
        if ($in && /^\s+(\S.*?)\(in .+\)\s+(\d+)\s*$/) {
            my $count = $2;                 # ⚠ 先存 count：下面 s/// 会清空 $1/$2
            my $k = $1; $k =~ s/\s+$//;     # 去尾随空格（sample frame 名后有对齐空格）
            $c{$k} += $count
        }
        END {
            for $k (sort { $c{$b} <=> $c{$a} } keys %c) {
                last if $n >= '"$TOPN"';
                print "| $c{$k} | `$k` |\n"; $n++
            }
        }' "$STACKS")"
else
    # perf report --stdio:  45.23%  imapsServer  imapsServer  func
    TOP_TABLE="$(perl -ne '
        BEGIN { $n = 0 }
        if (/^\s+([0-9.]+)%\s+\S+\s+\S+\s+(.+?)\s*$/) { $c{$2} += $1 }
        END {
            for $k (sort { $c{$b} <=> $c{$a} } keys %c) {
                last if $n >= '"$TOPN"';
                printf "| %.2f%% | `%s` |\n", $c{$k}, $k; $n++
            }
        }' "$STACKS")"
fi

# ── 7. 写报告 ─────────────────────────────────────────────────────────────
{
    echo ""
    # ⚠ 全角标点紧跟裸 $VAR 会被 bash 当成变量名一部分（UTF-8 locale 下），
    # set -u 时报 "VAR�: unbound"——一律用 ${VAR} 大括号包裹。
    echo "## Profile 热点（${STAMP}，${UNAME}，采样 ${SAMPLE_SECS}s，负载: ${LOAD_ARGS}）"
    echo ""
    echo "$THROUGHPUT"
    echo ""
    echo "| 采样计数 | 热点函数 |"
    echo "|---------|---------|"
    echo "$TOP_TABLE"
    echo ""
} >> "$REPORT"

echo "[profile] done — 热点 top-$TOPN 已追加到 $REPORT"
echo "----- 采样 top-$TOPN -----"
echo "$TOP_TABLE"
