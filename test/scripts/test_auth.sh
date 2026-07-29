#!/bin/bash
# SMTP AUTH 测试脚本
# 用法: ./test_auth.sh [email] [password]
# 默认: t1@hgmail.hgmail.xin / 123456

set -e

HOST="${SMTP_HOST:-127.0.0.1}"
PORT="${SMTP_PORT:-465}"
EMAIL="${1:-t1@hgmail.hgmail.xin}"
PASSWORD="${2:-123456}"

# 判断是完整邮箱还是本地部分（AUTH LOGIN 只用本地部分也能过，服务端会补全）
LOCAL_PART="${EMAIL%%@*}"

echo "============================================"
echo "SMTP AUTH 测试"
echo "  服务器: ${HOST}:${PORT}"
echo "  邮箱:   ${EMAIL}"
echo "  本地部分: ${LOCAL_PART}"
echo "  密码:   ${PASSWORD}"
echo "============================================"

# ---- 辅助函数：base64 编码 ----
b64() { echo -n "$1" | base64; }

# ---- AUTH LOGIN 测试 ----
test_auth_login() {
    echo ""
    echo ">>> [AUTH LOGIN] 开始测试 <<<"
    (
        # Wait for 220 greeting
        sleep 0.5
        echo "EHLO test.local"
        sleep 0.3
        echo "AUTH LOGIN"
        sleep 0.3
        # 发送用户名（本地部分即可，服务端自动补全域名）
        b64 "${LOCAL_PART}"
        sleep 0.3
        # 发送密码
        b64 "${PASSWORD}"
        sleep 0.3
        echo "QUIT"
    ) | openssl s_client -connect "${HOST}:${PORT}" -quiet 2>/dev/null
}

# ---- AUTH PLAIN 测试 ----
test_auth_plain() {
    echo ""
    echo ">>> [AUTH PLAIN] 开始测试 <<<"
    # PLAIN 格式: \0username\0password → base64
    PLAIN_RAW=$(printf '\0%s\0%s' "${LOCAL_PART}" "${PASSWORD}")
    PLAIN_B64=$(echo -n "${PLAIN_RAW}" | base64)

    (
        sleep 0.5
        echo "EHLO test.local"
        sleep 0.3
        echo "AUTH PLAIN"
        sleep 0.3
        # 接收 334 后发送 PLAIN 凭证
        echo "${PLAIN_B64}"
        sleep 0.3
        echo "QUIT"
    ) | openssl s_client -connect "${HOST}:${PORT}" -quiet 2>/dev/null
}

# ---- 依次执行 ----
test_auth_login
test_auth_plain

echo ""
echo "============================================"
echo "测试完成"
echo "============================================"
