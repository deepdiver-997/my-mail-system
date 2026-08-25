#!/bin/bash
set -e
# 清理测试环境
rm -rf "/tmp/protorelay_test"
rm -rf "/Users/zhuhongrui/code/c++/project/mail-system/v8/test/config/crt" "/Users/zhuhongrui/code/c++/project/mail-system/v8/test/config/dkim" "/Users/zhuhongrui/code/c++/project/mail-system/v8/test/config/sql_init" "/Users/zhuhongrui/code/c++/project/mail-system/v8/test/config/sql"
rm -f "/Users/zhuhongrui/code/c++/project/mail-system/v8/test/config/create_tables.sql"
rm -f "/Users/zhuhongrui/code/c++/project/mail-system/v8/test/config/smtps_mock.json" "/Users/zhuhongrui/code/c++/project/mail-system/v8/test/config/smtps_real.json"
rm -f "/Users/zhuhongrui/code/c++/project/mail-system/v8/test/config/imaps_mock.json" "/Users/zhuhongrui/code/c++/project/mail-system/v8/test/config/imaps_real.json"
rm -f "/Users/zhuhongrui/code/c++/project/mail-system/v8/test/config/db_config.json" "/Users/zhuhongrui/code/c++/project/mail-system/v8/test/config/router_config.json"
pkill -f "smtpsServer.*test/config" 2>/dev/null || true
pkill -f "imapsServer.*test/config" 2>/dev/null || true
echo "Test environment cleaned."
