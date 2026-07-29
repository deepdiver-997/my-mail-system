#!/bin/bash
# 清理测试数据 — mail 目录、DB（保留 users 表）、tmp 文件、进程
# 用法:
#   bash test/scripts/cleanup.sh          # 清理 mail/attachments + /tmp + 杀进程
#   bash test/scripts/cleanup.sh --all    # 额外清理 DB 非用户表
#   bash test/scripts/cleanup.sh --hard   # 清理 DB 所有表 + 重建 schema

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TEST_DIR="$(dirname "$SCRIPT_DIR")"
PROJECT_DIR="$(dirname "$TEST_DIR")"
DB_USER="${DB_USER:-mail_test}"
DB_PASS="${DB_PASS:-abjskKA09qjf}"
DB_HOST="${DB_HOST:-localhost}"
DB_NAME="${DB_NAME:-mail}"

echo "=== Cleanup ==="

# 1. 邮件落盘（rm -rf 比 find -delete 快 100x，避免遍历海量文件卡死）
echo -n "Mail files... "
rm -rf "$PROJECT_DIR/mail"          && mkdir -p "$PROJECT_DIR/mail"
rm -rf "$PROJECT_DIR/attachments"   && mkdir -p "$PROJECT_DIR/attachments"
rm -rf /tmp/protorelay_test
rm -rf /tmp/fsm_bench_mail /tmp/fsm_bench_att
rm -rf /tmp/smtps_fsm_test_mail /tmp/smtps_fsm_test_att
rm -rf /tmp/imaps_fsm_test_mail /tmp/imaps_fsm_test_att
rm -f  /tmp/smtps_fsm_test.log /tmp/imaps_fsm_test.log
echo "done"

# 2. 数据库（非用户表）
if command -v mysql &>/dev/null; then
    MYSQL="mysql -u $DB_USER -p$DB_PASS -h $DB_HOST $DB_NAME"

    if [ "${1:-}" = "--hard" ]; then
        # 完全重建所有表（包括 users）
        echo -n "Database (hard reset + re-init)... "
        $MYSQL -e "
            DROP TABLE IF EXISTS mail_mailbox, mail_recipients, mail_outbox, attachments, mails, mailboxes, users;
        " 2>/dev/null || true

        SCHEMA="${PROJECT_DIR}/config/sql/create_tables.sql"
        if [ -f "$SCHEMA" ]; then
            $MYSQL < "$SCHEMA" 2>/dev/null || true
        fi
        echo "done"
    elif [ "${1:-}" = "--all" ]; then
        # 清空非用户表（保留 users 账号）
        echo -n "Database (truncate non-user tables)... "
        $MYSQL -e "
            SET FOREIGN_KEY_CHECKS=0;
            TRUNCATE TABLE mail_mailbox;
            TRUNCATE TABLE mail_recipients;
            TRUNCATE TABLE mail_outbox;
            TRUNCATE TABLE attachments;
            TRUNCATE TABLE mails;
            SET FOREIGN_KEY_CHECKS=1;
        " 2>/dev/null || true
        echo "done"
    fi
fi

# 3. 进程
pkill -9 -f smtpsServer 2>/dev/null || true
pkill -9 -f imapsServer 2>/dev/null || true
pkill -9 -f fsm_bench 2>/dev/null || true
pkill -9 -f smtp_client 2>/dev/null || true
echo "  processes killed"

echo "=== Cleanup complete ==="
echo "  mail/attachments dirs reset (rm -rf)"
[ "${1:-}" = "--all" ]  && echo "  DB non-user tables truncated"
[ "${1:-}" = "--hard" ] && echo "  DB fully reset + re-initialized"
[ -z "${1:-}" ]          && echo "  (use --all for DB cleanup, --hard for full DB reset)"
