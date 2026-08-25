-- e2e relay 测试种子数据
-- 由 test/e2e/test_outbound_relay.py 自动加载
-- 用途：保证 alice@a.local / bob@b.local 用户存在（FSM 收件人检查 + 认证需要）
--
-- 为什么不在 setup_test_env.py 里写？
--  e2e 进程级测试用 alice@a.local 当 sender + auth user；
--  bob@b.local 是外部域收件人（不落 B 盘也行，但若以后 B 改 MTA 模式就需要
--  bob 用户存在）。
--  把 seed 放本目录让 e2e 自治：跑 e2e 不依赖 setup_test_env 任何顺序。

-- ── alice@a.local：发件人 + A 端认证账号 ─────────────────────
INSERT IGNORE INTO users (mail_address, password, name, register_time)
VALUES (
    'alice@a.local',
    -- 密码字段：smtpsServer 的 auth backend 用 plaintext match (perf_mode 跳 SPF/DKIM 后)
    'e2e_password',
    'e2e relay test sender',
    NOW()
);

-- ── bob@b.local：B 端外部收件人（MTA 模式时需要）───────────
-- 当前 B 是 auth_policy=on（不依赖），保留以便未来 B 切 MTA 后用。
INSERT IGNORE INTO users (mail_address, password, name, register_time)
VALUES (
    'bob@b.local',
    'e2e_password',
    'e2e relay test recipient',
    NOW()
);

-- ── alice 的 INBOX mailbox（A 是 local 域，alice 收件会入 INBOX）────
-- 注意：如果 users.id 是 AUTO_INCREMENT，mailboxes.user_id 关联
INSERT IGNORE INTO mailboxes (user_id, mailbox_name, uid_next, uid_validity)
SELECT id, 'INBOX', 1, UNIX_TIMESTAMP()
FROM users
WHERE mail_address = 'alice@a.local';
