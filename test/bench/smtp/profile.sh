#!/usr/bin/env bash
# SMTP 压测 profiling 闭环 —— 高负载下采样找热点，输出 top-N 热点表。
#
# 流程: 起 release smtpsServer → 后台压测（smtp_client 发信）→ 采样服务 PID N 秒
#       → 折叠成 top-N 热点表 → 追加进 smtp/REPORT.md。
#
# 平台: macOS `sample`（内置）/ Linux `perf`（见 imap/profile.sh 同款说明）。
#
# ⚠ SMTP 是写路径：压测会灌信，mailbox 会涨。profile 前确认用的是 bench 配置
#   （perf_mode/persist 上限调大），跑完按需清理（test/scripts/cleanup.sh）。
#
# 用法:
#   ./profile.sh --config config/smtpsConfig.json
#   ./profile.sh --load-args "--t 16 --msgs 8000 --pipe --reuse"
#   ./profile.sh --sample-secs 10 --topn 20

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"   # smtp/ → bench/ → test/ → 根
BENCH_DIR="$ROOT/test/bench"
REPORT="$BENCH_DIR/smtp/REPORT.md"

SERVER="$ROOT/build-release/smtpsServer"
CONFIG="${CONFIG:-$ROOT/config/smtpsConfig.json}"
# 默认负载要跑得比采样窗口久。smtp_client --msgs 是每线程消息数（总 = 线程×msgs）。
# 12k msg/s × 10s ≈ 120k 封，16 线程 × 8000 = 128k 够覆盖。
LOAD_ARGS="${LOAD_ARGS:---t 16 --msgs 8000 --pipe --reuse}"
SAMPLE_SECS=10
TOPN=20

while [[ $# -gt 0 ]]; do
    case "$1" in
        --server)    SERVER="$2"; shift 2 ;;
        --config)    CONFIG="$2"; shift 2 ;;
        --load-args) LOAD_ARGS="$2"; shift 2 ;;
        --sample-secs) SAMPLE_SECS="$2"; shift 2 ;;
        --topn)      TOPN="$2"; shift 2 ;;
        *) echo "unknown arg $1" >&2; exit 1 ;;
    esac
done

if [[ ! -x "$SERVER" ]]; then
    if [[ -x "$ROOT/build/smtpsServer" ]]; then
        SERVER="$ROOT/build/smtpsServer"
        echo "[warn] release binary not found — using Debug $SERVER（热点不可靠）" >&2
    else
        echo "[error] no smtpsServer binary. Build: cmake --build build-release --target smtpsServer -j 4" >&2
        exit 1
    fi
fi
CLIENT="$ROOT/build/smtp_client"
[[ -x "$CLIENT" ]] || { echo "[error] smtp_client not built: cmake --build build --target smtp_client -j 4" >&2; exit 1; }
[[ -f "$CONFIG" ]] || { echo "[error] config not found: $CONFIG" >&2; exit 1; }

STAMP="$(date '+%Y-%m-%d %H:%M')"
UNAME="$(uname -s)"
STACKS="/tmp/smtp_profile_stacks_$$.txt"
LOAD_OUT="/tmp/smtp_profile_load_$$.out"
SRV_PID=""; LOAD_PID=""

cleanup() { [[ -n "$SRV_PID" ]] && kill "$SRV_PID" 2>/dev/null || true; [[ -n "$LOAD_PID" ]] && kill "$LOAD_PID" 2>/dev/null || true; }
trap cleanup EXIT

echo "[profile] server: $SERVER"
"$SERVER" -c "$CONFIG" > /tmp/smtp_profile_server.log 2>&1 &
SRV_PID=$!
sleep 2
kill -0 "$SRV_PID" 2>/dev/null || { echo "[error] server failed (see /tmp/smtp_profile_server.log)" >&2; exit 1; }

echo "[profile] load: smtp_client $LOAD_ARGS"
"$CLIENT" $LOAD_ARGS > "$LOAD_OUT" 2>&1 &
LOAD_PID=$!
sleep 2

echo "[profile] sampling server PID $SRV_PID for ${SAMPLE_SECS}s ..."
if [[ "$UNAME" == "Darwin" ]]; then
    sample "$SRV_PID" "$SAMPLE_SECS" > "$STACKS" 2>&1 \
        || { echo "[error] sample failed" >&2; exit 1; }
elif [[ "$UNAME" == "Linux" ]]; then
    PERF_DATA="/tmp/smtp_perf_profile_$$.data"
    perf record -F 99 -g -p "$SRV_PID" -o "$PERF_DATA" -- sleep "$SAMPLE_SECS" >/dev/null 2>&1 \
        || { echo "[error] perf record failed (need root? sudo ./profile.sh)" >&2; exit 1; }
    perf report -i "$PERF_DATA" --stdio --sort symbol --no-children > "$STACKS" 2>&1 || true
else
    echo "[error] unsupported platform $UNAME" >&2; exit 1
fi

wait "$LOAD_PID" 2>/dev/null || true
LOAD_PID=""
THROUGHPUT="$(grep -oE "rate=[0-9]+ msg/s" "$LOAD_OUT" | tail -1 || true)"

if [[ "$UNAME" == "Darwin" ]]; then
    TOP_TABLE="$(perl -ne '
        BEGIN { $n = 0 }
        if (/Sort by top of stack, same collapsed/) { $in = 1; next }
        if ($in && /^\s+(\S.*?)\(in .+\)\s+(\d+)\s*$/) {
            my $count = $2;                 # ⚠ 先存 count：下面 s/// 会清空 $1/$2
            my $k = $1; $k =~ s/\s+$//;
            $c{$k} += $count
        }
        END {
            for $k (sort { $c{$b} <=> $c{$a} } keys %c) {
                last if $n >= '"$TOPN"';
                print "| $c{$k} | `$k` |\n"; $n++
            }
        }' "$STACKS")"
else
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

{
    echo ""
    echo "## SMTP Profile 热点（${STAMP}，${UNAME}，采样 ${SAMPLE_SECS}s，负载: ${LOAD_ARGS}）"
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
