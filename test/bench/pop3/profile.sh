#!/usr/bin/env bash
# POP3 读路径 profiling 闭环 —— 高负载下采样找热点，输出 top-N 热点表。
#
# 流程: 起 release pop3Server → 后台压测（pop3_client STAT+LIST+RETR）→ 采样服务
#       PID N 秒 → 折叠成 top-N 热点表 → 追加进 pop3/REPORT.md。
#
# 平台: macOS `sample`（内置）/ Linux `perf`（见 imap/profile.sh 同款说明）。
# POP3 是读路径，不产生新数据，无需清理。
#
# 用法:
#   ./profile.sh
#   ./profile.sh --config config/pop3Config.json
#   ./profile.sh --load-args "--t 16 --conns 4 --rounds 800 --mails 5"
#   ./profile.sh --sample-secs 10 --topn 20

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"   # pop3/ → bench/ → test/ → 根
BENCH_DIR="$ROOT/test/bench"
REPORT="$BENCH_DIR/pop3/REPORT.md"

SERVER="$ROOT/build-release/pop3Server"
# 默认用 DB-backed bench 配置（config/pop3Config.json 无 DB，查不到用户）
CONFIG="${CONFIG:-/tmp/pop3_bench_config.json}"
# 默认负载：并发 ≤ 用户数（POP3 单会话锁，超出抢锁失败）。5 用户 × 4 并发 ×
# 500 轮（STAT+LIST+RETR 1:5，多命令往返较慢）≈ 覆盖 10s 采样。
POP3_USERS="${POP3_USERS:-test2@scut.email,t1@scut.email,test@scut.email,alice@a.local,bob@b.local}"
# 用户列表无空格，直接展开（unquoted $LOAD_ARGS 会 word-split）；有空格时用 --load-args 覆盖
LOAD_ARGS="${LOAD_ARGS:---port 1110 --users ${POP3_USERS} --t 4 --conns 1 --rounds 500 --mails 5}"
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
    if [[ -x "$ROOT/build/pop3Server" ]]; then
        SERVER="$ROOT/build/pop3Server"
        echo "[warn] release binary not found — using Debug $SERVER（热点不可靠）" >&2
    else
        echo "[error] no pop3Server binary. Build: cmake --build build-release --target pop3Server -j 4" >&2
        exit 1
    fi
fi
CLIENT="$ROOT/build/pop3_client"
[[ -x "$CLIENT" ]] || { echo "[error] pop3_client not built: cmake --build build --target pop3_client -j 4" >&2; exit 1; }
[[ -f "$CONFIG" ]] || { echo "[error] config not found: $CONFIG" >&2; exit 1; }

STAMP="$(date '+%Y-%m-%d %H:%M')"
UNAME="$(uname -s)"
STACKS="/tmp/pop3_profile_stacks_$$.txt"
LOAD_OUT="/tmp/pop3_profile_load_$$.out"
SRV_PID=""; LOAD_PID=""

cleanup() { [[ -n "$SRV_PID" ]] && kill "$SRV_PID" 2>/dev/null || true; [[ -n "$LOAD_PID" ]] && kill "$LOAD_PID" 2>/dev/null || true; }
trap cleanup EXIT

echo "[profile] server: $SERVER"
"$SERVER" -c "$CONFIG" > /tmp/pop3_profile_server.log 2>&1 &
SRV_PID=$!
sleep 2
kill -0 "$SRV_PID" 2>/dev/null || { echo "[error] server failed (see /tmp/pop3_profile_server.log)" >&2; exit 1; }

echo "[profile] load: pop3_client $LOAD_ARGS"
"$CLIENT" $LOAD_ARGS > "$LOAD_OUT" 2>&1 &
LOAD_PID=$!
sleep 2

echo "[profile] sampling server PID $SRV_PID for ${SAMPLE_SECS}s ..."
if [[ "$UNAME" == "Darwin" ]]; then
    sample "$SRV_PID" "$SAMPLE_SECS" > "$STACKS" 2>&1 \
        || { echo "[error] sample failed" >&2; exit 1; }
elif [[ "$UNAME" == "Linux" ]]; then
    PERF_DATA="/tmp/pop3_perf_profile_$$.data"
    perf record -F 99 -g -p "$SRV_PID" -o "$PERF_DATA" -- sleep "$SAMPLE_SECS" >/dev/null 2>&1 \
        || { echo "[error] perf record failed (need root? sudo ./profile.sh)" >&2; exit 1; }
    perf report -i "$PERF_DATA" --stdio --sort symbol --no-children > "$STACKS" 2>&1 || true
else
    echo "[error] unsupported platform $UNAME" >&2; exit 1
fi

wait "$LOAD_PID" 2>/dev/null || true
LOAD_PID=""
THROUGHPUT="$(grep -oE "throughput=[0-9]+ rounds/s" "$LOAD_OUT" | tail -1 || true)"

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
    echo "## POP3 Profile 热点（${STAMP}，${UNAME}，采样 ${SAMPLE_SECS}s，负载: ${LOAD_ARGS}）"
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
