#!/usr/bin/env bash
# ProtoRelay 一键交叉编译 + 部署
# 用法: ./deploy.sh           增量编译部署
#       ./deploy.sh clean     全量重新编译

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SERVER="root@120.24.169.213"
TARGET_DIR="/opt/smtpServer"
SYSROOT="${HOME}/.protorelay/sysroot/usr"
JOBS="4"
BUILD_TYPE="Release"
MODE="cross-x64 object-only"

# --- 参数解析 ---
CLEAN=""
for arg in "$@"; do
    case "$arg" in
        clean|--clean) CLEAN="clean" ;;
        -h|--help)
            echo "Usage: $0 [clean]"
            echo "  clean    全量重新编译"
            exit 0 ;;
        *) echo "Unknown: $arg"; exit 1 ;;
    esac
done

# --- Step 1: 确保 sysroot 头文件存在 ---
if [ ! -d "$SYSROOT/spdlog" ] || [ ! -d "$SYSROOT/fmt" ]; then
    echo "[1/6] Syncing sysroot headers from server..."
    mkdir -p "$SYSROOT"
    ssh "$SERVER" "tar czf - -C /usr/include spdlog fmt" | tar xzf - -C "$SYSROOT"
    echo "  Sysroot ready: $(find "$SYSROOT" -type f | wc -l) files"
else
    echo "[1/6] Sysroot headers OK (skip)"
fi

# --- Step 2: 交叉编译（build.sh 自动处理 Homebrew 屏蔽、SYSROOT_INCLUDE 等） ---
echo "[2/6] Cross-compiling ($BUILD_TYPE, $JOBS jobs, ${CLEAN:-incremental})..."
bash "$SCRIPT_DIR/build.sh" "$BUILD_TYPE" $CLEAN "$JOBS" $MODE

OBJ_DIR="$SCRIPT_DIR/artifacts/linux-x86_64/$BUILD_TYPE/obj"
OBJ_COUNT=$(find "$OBJ_DIR" -type f -name '*.o' | wc -l | tr -d ' ')
echo "  Compiled $OBJ_COUNT object files"

# --- Step 3: 上传 ---
echo "[3/6] Uploading to $SERVER..."
ssh "$SERVER" "rm -rf $TARGET_DIR/obj $TARGET_DIR/smtpsServer $TARGET_DIR/imapsServer"
scp -r "$OBJ_DIR" "$SERVER:$TARGET_DIR/" 2>&1
scp "$SCRIPT_DIR/link.sh" "$SERVER:$TARGET_DIR/" 2>&1
echo "  Uploaded"

# --- Step 4: 链接 ---
echo "[4/6] Linking binaries on server..."
ssh "$SERVER" "cd $TARGET_DIR && \
    bash link.sh obj/CMakeFiles/smtpsServer_obj.dir/test/smtps_test.cpp.o \
        -o smtpsServer --compiler g++-13 \
        --exclude imaps_test --exclude mail_server 2>&1 | tail -1"
ssh "$SERVER" "cd $TARGET_DIR && \
    bash link.sh obj/CMakeFiles/imapsServer_obj.dir/test/imaps_test.cpp.o \
        -o imapsServer --compiler g++-13 \
        --exclude smtps_test --exclude mail_server 2>&1 | tail -1"
echo "  Linked"

# --- Step 5: 同步配置和辅助文件 ---
echo "[5/6] Syncing config files..."
rsync -az \
    --exclude='build' --exclude='*.o' \
    --exclude='logs/*' --exclude='mail/*' --exclude='attachments/*' \
    --exclude='db_config.json' \
    --exclude='smtpsConfig.json' --exclude='imapsConfig.json' \
    "$SCRIPT_DIR/config/" "$SERVER:$TARGET_DIR/config/" 2>&1 | tail -1 || true
scp "$SCRIPT_DIR/test/hash_tool.cpp" "$SERVER:$TARGET_DIR/" 2>&1 || true
# 确保 hash_tool 已编译
ssh "$SERVER" "cd $TARGET_DIR && [ -x hash_tool ] || g++ -std=c++17 -o hash_tool hash_tool.cpp -lssl -lcrypto 2>/dev/null"
echo "  Synced"

# --- Step 6: 重启服务 ---
echo "[6/6] Restarting services..."
ssh "$SERVER" "systemctl restart smtpserver imapserver 2>&1 && \
    sleep 2 && \
    echo '  SMTP:' && systemctl status smtpserver --no-pager | head -3 | tail -1 && \
    echo '  IMAP:' && systemctl status imapserver --no-pager | head -3 | tail -1 && \
    echo '  Ports:' && ss -tlnp | grep -E ':(25|465|143|993) ' | awk '{print \"    \"\$4}' | sort"

echo ""
echo "Deploy done."
