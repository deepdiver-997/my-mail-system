#!/usr/bin/env bash
# ProtoRelay 一键交叉编译 + 部署
# 用法: ./deploy.sh           增量编译部署
#       ./deploy.sh clean     全量重新编译

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# 真实服务器地址通过环境变量 DEPLOY_SERVER 提供（默认占位符，真实值见 docs/local/server-credentials.md）
SERVER="${DEPLOY_SERVER:-root@<SERVER_IP>}"
TARGET_DIR="/opt/smtpServer"
SYSROOT="${HOME}/.protorelay/sysroot/usr"
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)"
BUILD_TYPE="Release"
MODE="cross-x64 object-only"
# 自动备份保留份数（只回收本脚本生成的 .bak-<日期>-<时刻>，手工打标签的备份不动）
KEEP_BACKUPS=5

# --- SSH 连接复用：整个部署 6+ 次往返共用一条连接 ---
# ControlPath 用 ~/.ssh/cm-%C：%C 是连接参数的短哈希。
# 不要用 $TMPDIR —— macOS 的 /var/folders/... 很长，加上 user@host:port 容易
# 超过 unix socket 路径上限（约 104 字节），复用会静默失效。
mkdir -p "$HOME/.ssh"
SSH_OPTS=(-o ControlMaster=auto -o "ControlPath=$HOME/.ssh/cm-%C" -o ControlPersist=60s)
# 远端命令统一带 pipefail：否则 `cmd | tail -1` 会把 cmd 的失败吞成 0
ssh_strict() { ssh "${SSH_OPTS[@]}" "$SERVER" "set -euo pipefail; $*"; }
cleanup_ssh() { ssh "${SSH_OPTS[@]}" -O exit "$SERVER" >/dev/null 2>&1 || true; }
trap cleanup_ssh EXIT

# --- 参数解析 ---
CLEAN=""
DRY_RUN=""
for arg in "$@"; do
    case "$arg" in
        clean|--clean) CLEAN="clean" ;;
        --dry-run) DRY_RUN="1" ;;
        -h|--help)
            echo "Usage: $0 [clean] [--dry-run]"
            echo "  clean      全量重新编译"
            echo "  --dry-run  构建/上传/链接/校验全跑，但停在替换线上二进制之前。"
            echo "             用于验证部署机制本身，不影响线上服务。"
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

# --- Step 3: 上传到暂存目录 ---
# 关键：不碰线上正在服役的 obj/ 和二进制。中途任何一步失败，线上仍是可用状态。
echo "[3/6] Uploading to $SERVER (staging)..."
ssh_strict "mkdir -p '$TARGET_DIR/obj.new'"
# --link-dest 是增量的关键：obj.new 每次部署结束都会被 mv 成 obj，下次又是空目录，
# 没有参照物的话每次都是全量传输，等于白换 rsync。指向上一版 obj/ 后，
# 未变化的 .o 直接在服务器上硬链接过来，网络上只传真正变了的那些。
rsync -az --delete --link-dest="$TARGET_DIR/obj" -e "ssh ${SSH_OPTS[*]}" \
    "$OBJ_DIR/" "$SERVER:$TARGET_DIR/obj.new/"
rsync -az -e "ssh ${SSH_OPTS[*]}" \
    "$SCRIPT_DIR/link.sh" "$SERVER:$TARGET_DIR/"
echo "  Uploaded to obj.new/ (incremental via --link-dest)"

# --- Step 4: 链接到 *.new ---
# --obj-root 必须显式给：否则 link.sh 会收集 $TARGET_DIR 下**所有** .o
# （服务器上还留着历史的 build-obj/），两份对象树混链 → 重复符号。
echo "[4/6] Linking binaries on server..."
ssh_strict "cd '$TARGET_DIR' && \
    bash link.sh obj.new/CMakeFiles/smtpsServer_obj.dir/test/smtps_test.cpp.o \
        --obj-root obj.new \
        -o smtpsServer.new --compiler g++-13 \
        --exclude imaps_test --exclude mail_server 2>&1 | tail -1"
ssh_strict "cd '$TARGET_DIR' && \
    bash link.sh obj.new/CMakeFiles/imapsServer_obj.dir/test/imaps_test.cpp.o \
        --obj-root obj.new \
        -o imapsServer.new --compiler g++-13 \
        --exclude smtps_test --exclude mail_server 2>&1 | tail -1"

# 产物校验：链接曾经静默失败过，这里必须挡住
ssh "${SSH_OPTS[@]}" "$SERVER" bash -s <<REMOTE
set -euo pipefail
cd '$TARGET_DIR'
for b in smtpsServer.new imapsServer.new; do
    [ -x "\$b" ] || { echo "missing or not executable: \$b" >&2; exit 1; }
    [ -s "\$b" ] || { echo "empty binary: \$b" >&2; exit 1; }
done
echo '  Linked and verified:'
ls -la smtpsServer.new imapsServer.new | awk '{print "    "\$5, \$9}'
REMOTE

# --- Step 5: 同步配置和辅助文件 ---
echo "[5/6] Syncing config files..."
# 这里**刻意不加 --delete**。config/ 下有服务器侧独有、且不该进仓库的东西：
#   dkim/*.private.pem  （DKIM 私钥，删了外发邮件签名全废）
#   crt/*              （TLS 证书及其备份）
#   db_config_mail2.json、各种 *.bak
# 加 --delete 会把它们全部清掉。obj.new/ 那边可以 --delete，因为那是纯构建产物。
rsync -az -e "ssh ${SSH_OPTS[*]}" \
    --exclude='build' --exclude='*.o' \
    --exclude='logs/*' --exclude='mail/*' --exclude='attachments/*' \
    --exclude='db_config.json' \
    --exclude='smtpsConfig.json' --exclude='imapsConfig.json' \
    "$SCRIPT_DIR/config/" "$SERVER:$TARGET_DIR/config/"
rsync -az -e "ssh ${SSH_OPTS[*]}" \
    "$SCRIPT_DIR/test/hash_tool.cpp" "$SERVER:$TARGET_DIR/"
# hash_tool 体积极小，仍在服务器上编译；如需彻底避免服务器编译可改为交叉产出后上传
ssh_strict "cd '$TARGET_DIR' && { [ -x hash_tool ] || g++ -std=c++17 -o hash_tool hash_tool.cpp -lssl -lcrypto; }"
echo "  Synced"

# --- Step 6: 备份 + 原子替换 + 重启 + 冒烟 + 失败自动回滚 ---
if [ -n "$DRY_RUN" ]; then
    echo "[6/6] DRY RUN — 停在替换之前，线上二进制未被触碰。"
    ssh_strict "cd '$TARGET_DIR' && \
        echo '  现役:' && ls -la smtpsServer imapsServer 2>/dev/null | awk '{print \"    \"\$5, \$9}' && \
        echo '  待替换:' && ls -la smtpsServer.new imapsServer.new | awk '{print \"    \"\$5, \$9}'"
    echo ""
    echo "空跑完成：构建、上传、链接、产物校验均通过。"
    echo "清理暂存物：ssh \$SERVER 'cd $TARGET_DIR && rm -f *.new && rm -rf obj.new'"
    exit 0
fi

# swap / restart / smoke / rollback 放在同一个远端脚本里：
# 回滚需要用到本次备份的时间戳，跨 ssh 调用传递既麻烦又容易在中途断连时失配。
echo "[6/6] Swapping in new binaries, restarting, smoke-testing..."
ssh "${SSH_OPTS[@]}" "$SERVER" bash -s <<REMOTE
set -euo pipefail
cd '$TARGET_DIR'

TS=\$(date +%Y%m%d-%H%M%S)
BACKED_UP=""

# 备份现役二进制（首次部署时可能不存在）
for b in smtpsServer imapsServer; do
    if [ -f "\$b" ]; then
        cp -p "\$b" "\$b.bak-\$TS"
        BACKED_UP="yes"
    fi
done

# 原子替换：mv 在同一文件系统上原子；替换正在运行的可执行文件是安全的，
# 运行中的进程持有旧 inode，直到 restart 才真正切换。
for b in smtpsServer imapsServer; do
    mv "\$b.new" "\$b"
done
echo "  Backed up as *.bak-\$TS, new binaries in place"

# 对象树轮换：保留上一份便于排查
rm -rf obj.old
if [ -d obj ]; then
    mv obj obj.old
fi
mv obj.new obj

rollback() {
    echo "  !! 冒烟失败，回滚到 *.bak-\$TS" >&2
    if [ -z "\$BACKED_UP" ]; then
        echo "  !! 本次是首次部署，没有可回滚的备份；服务已停在新二进制上" >&2
        return
    fi
    for b in smtpsServer imapsServer; do
        if [ -f "\$b.bak-\$TS" ]; then
            cp -p "\$b.bak-\$TS" "\$b"
        fi
    done
    systemctl restart smtpserver imapserver || true
    sleep 2
    echo "  已回滚并重启" >&2
}

# --- 重启 ---
systemctl restart smtpserver imapserver
sleep 2
for svc in smtpserver imapserver; do
    if ! systemctl is-active --quiet "\$svc"; then
        echo "  !! \$svc 未能启动" >&2
        systemctl status "\$svc" --no-pager | head -15 >&2
        rollback
        exit 1
    fi
done

# --- 冒烟：真实建连跑一轮协议交互 ---
# 只验明文端口（25/143）：TLS 端口要拉 openssl，而问候+EHLO 已足以证明
# 进程在监听、FSM 起得来、能正常应答 —— 恰好覆盖"能启动但一连就崩"这类。
smoke_smtp() {
    local line
    exec 3<>/dev/tcp/127.0.0.1/25 || return 1
    IFS= read -r -t 5 line <&3 || { exec 3<&- 3>&-; return 1; }
    case "\$line" in
        220*) ;;
        *) echo "  !! SMTP 问候异常: \$line" >&2; exec 3<&- 3>&-; return 1 ;;
    esac
    printf 'EHLO deploy-smoke.local\r\n' >&3
    IFS= read -r -t 5 line <&3 || { exec 3<&- 3>&-; return 1; }
    case "\$line" in
        250*) ;;
        *) echo "  !! SMTP EHLO 异常: \$line" >&2; exec 3<&- 3>&-; return 1 ;;
    esac
    printf 'QUIT\r\n' >&3
    exec 3<&- 3>&-
    return 0
}

smoke_imap() {
    local line
    exec 4<>/dev/tcp/127.0.0.1/143 || return 1
    IFS= read -r -t 5 line <&4 || { exec 4<&- 4>&-; return 1; }
    case "\$line" in
        "* OK"*) ;;
        *) echo "  !! IMAP 问候异常: \$line" >&2; exec 4<&- 4>&-; return 1 ;;
    esac
    printf 'a1 CAPABILITY\r\n' >&4
    IFS= read -r -t 5 line <&4 || { exec 4<&- 4>&-; return 1; }
    case "\$line" in
        "* CAPABILITY"*) ;;
        *) echo "  !! IMAP CAPABILITY 异常: \$line" >&2; exec 4<&- 4>&-; return 1 ;;
    esac
    printf 'a2 LOGOUT\r\n' >&4
    exec 4<&- 4>&-
    return 0
}

if ! smoke_smtp; then rollback; exit 1; fi
echo "  Smoke SMTP  : 220 greeting + EHLO 250 OK"
if ! smoke_imap; then rollback; exit 1; fi
echo "  Smoke IMAP  : * OK greeting + CAPABILITY OK"

# 冒烟通过后才回收旧备份。
# 只回收本脚本生成的 .bak-<8位日期>-<6位时刻>；手工打标签的（如 .bak-tls13-...）
# 不匹配此模式，不会被误删。
# 注意 || true：无匹配时 ls 返回 1，配合 pipefail 会中断整个部署。
for b in smtpsServer imapsServer; do
    ls -1t "\$b".bak-????????-?????? 2>/dev/null \
        | tail -n +$((KEEP_BACKUPS + 1)) \
        | xargs -r rm -f || true
done

echo '  SMTP:' && systemctl status smtpserver --no-pager | head -3 | tail -1
echo '  IMAP:' && systemctl status imapserver --no-pager | head -3 | tail -1
echo '  Ports:' && ss -tlnp | grep -E ':(25|465|143|993) ' | awk '{print "    "\$4}' | sort
REMOTE

echo ""
echo "Deploy done."
echo "手工回滚：ssh \$DEPLOY_SERVER \"cd $TARGET_DIR && \\"
echo "            cp -p \\\$(ls -1t smtpsServer.bak-* | head -1) smtpsServer && \\"
echo "            systemctl restart smtpserver\""
