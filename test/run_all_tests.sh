#!/bin/bash
# 一键全套单测：构建（可跳过）+ 逐个运行 build/*_test + 汇总结果。
#
# 用法：
#   bash test/run_all_tests.sh                # 增量构建 Release + 全套运行
#   bash test/run_all_tests.sh Debug          # 指定构建类型
#   bash test/run_all_tests.sh --no-build     # 跳过构建，只跑已有二进制
#
# 环境变量：
#   RUN_DB_TESTS=0   跳过依赖真实 MySQL 的测试（默认 1 = 包含；本机已配 db_config 时应保持 1）
#
# 退出码：0 = 全部通过；1 = 有失败/构建失败。失败详情在 /tmp/<test>.log。
# 范围说明：本脚本覆盖 C++ 单测（test/unit）；e2e/bench/fuzz 不在其中
# （e2e 需起真实服务+DB，见 test/e2e；bench 见 test/bench）。

set -u
cd "$(dirname "$0")/.." || exit 1

BUILD_TYPE=Release
NO_BUILD=0
for a in "$@"; do
    case "$a" in
        Debug|Release|SafeRelease) BUILD_TYPE="$a" ;;
        --no-build|-n) NO_BUILD=1 ;;
        *) echo "未知参数: $a（支持 <Debug|Release|SafeRelease> / --no-build）"; exit 2 ;;
    esac
done

if [ "$NO_BUILD" -eq 0 ]; then
    bash build.sh "$BUILD_TYPE" || { echo "BUILD FAILED"; exit 1; }
fi

if [ ! -d build ]; then
    echo "build/ 不存在，先去掉 --no-build 跑一次构建"; exit 1
fi

RUN_DB_TESTS="${RUN_DB_TESTS:-1}"
SKIP_DB_RE='^(mariadb_async_test)$'

pass=0; fail=0; skip=0
failed_tests=()

shopt -s nullglob
for t in build/*_test; do
    [ -x "$t" ] || continue
    name=$(basename "$t")
    if [ "$RUN_DB_TESTS" = "0" ] && [[ "$name" =~ $SKIP_DB_RE ]]; then
        echo "SKIP  $name（依赖真实 MySQL，RUN_DB_TESTS=1 开启）"
        skip=$((skip+1))
        continue
    fi
    if "$t" > "/tmp/${name}.log" 2>&1; then
        echo "PASS  $name"
        pass=$((pass+1))
    else
        rc=$?
        echo "FAIL  $name (exit $rc，详情: /tmp/${name}.log)"
        fail=$((fail+1))
        failed_tests+=("$name")
    fi
done

echo
echo "======================================="
echo "全套单测: $pass 通过, $fail 失败, $skip 跳过"
if [ "$fail" -gt 0 ]; then
    printf '失败: %s\n' "${failed_tests[@]}"
    exit 1
fi
echo "ALL GREEN"
