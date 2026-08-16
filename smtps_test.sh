#!/bin/bash

# SMTP自动化测试脚本
# 用于测试SMTPS服务器的邮件接收功能

SMTP_SERVER="${SMTP_SERVER:-localhost}"
SMTP_PORT="${SMTP_PORT:-25}"
MAIL_FROM="${MAIL_FROM:-xxx@example.com}"
RCPT_TO="${RCPT_TO:-t1@example.com}"
MAIL_SUBJECT="${MAIL_SUBJECT:-SMTP Test Mail}"

# 颜色输出
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${YELLOW}=== SMTP自动化测试脚本 ===${NC}"
echo "测试服务器: $SMTP_SERVER:$SMTP_PORT"
echo "MAIL FROM: $MAIL_FROM"
echo "RCPT TO: $RCPT_TO"
echo ""

# 等待服务启动
echo -e "${YELLOW}等待SMTP服务器启动...${NC}"
sleep 0

# 测试连接
echo -e "${YELLOW}步骤1: 测试连接${NC}"
(echo -e "\nquit\n") | nc -Cv $SMTP_SERVER $SMTP_PORT
if [ $? -eq 0 ]; then
    echo -e "${GREEN}✓ 连接成功${NC}"
else
    echo -e "${RED}✗ 连接失败${NC}"
    exit 1
fi
echo ""

# 执行完整的SMTP对话
echo -e "${YELLOW}步骤2: 发送测试邮件${NC}"

(
    sleep 1
    printf "helo client\r\n"
    sleep 1
    printf "mail from:<$MAIL_FROM>\r\n"
    sleep 1
    printf "rcpt to:<$RCPT_TO>\r\n"
    sleep 1
    printf "data\r\n"
    sleep 1
    printf "Subject: $MAIL_SUBJECT\r\n"
    sleep 0.5
    printf "From: $MAIL_FROM\r\n"
    sleep 0.5
    printf "To: $RCPT_TO\r\n"
    sleep 0.5
    printf "\r\n"
    sleep 0.5
    printf "This is a test.\r\n"
    sleep 0.5
    printf ".\r\n"
    sleep 1
    printf "quit\r\n"
) | nc -Cv $SMTP_SERVER $SMTP_PORT

echo ""
echo -e "${GREEN}=== 测试完成 ===${NC}"
