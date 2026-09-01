-- SMTP 每日发信配额（2026-08-31）
--
-- 认证账号在 MAIL FROM 阶段原子占用当日配额：条件 UPDATE + ROW_COUNT
-- （见 TraditionalSmtpsFsm::check_send_quota_async）：
--   UPDATE users
--      SET sent_today = IF(sent_date = CURRENT_DATE, sent_today, 0) + 1,
--          sent_date  = CURRENT_DATE
--    WHERE mail_address = ?
--      AND IF(sent_date = CURRENT_DATE, sent_today, 0) < :daily_limit;
-- affected rows = 1 放行；= 0 表示当日配额已超（550 拒收）。
--
-- sent_date 跨天自动归零；NULL（首封）按 0 处理。配额值由配置
-- smtp_daily_send_limit 控制（0 = 不限）。
--
-- 注意：一次性迁移，已含列的表会报 duplicate column（MySQL 不支持
-- ADD COLUMN IF NOT EXISTS；MariaDB 支持但为跨方言统一用裸 ADD COLUMN）。
-- 幂等由"只跑一次"保证，与 migration_imap_uidnext 的 one-shot 语义一致。

ALTER TABLE users
  ADD COLUMN sent_today INT NOT NULL DEFAULT 0
    COMMENT '今日已发信数（每日配额计数，配置 smtp_daily_send_limit=0 则不限）',
  ADD COLUMN sent_date DATE NULL
    COMMENT '计数所属日期，跨天自动归零';
