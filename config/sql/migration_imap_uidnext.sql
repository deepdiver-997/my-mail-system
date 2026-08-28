-- mailbox_uidnext — per-mailbox UIDNEXT 高水位（RFC 3501 §2.3.1.1）
--
-- 旧实现 `SELECT MAX(mail_id)+1` 两个缺陷：
--   1) expunge 掉最大 mail_id 后 UIDNEXT 回落（违反"永不小于已用过的 UID"）
--   2) 并发/跨实例读者各自 MAX+1，读到同一值（非原子）
--
-- 新实现：每邮箱一行，atomic advance（见 TraditionalImapsFsm::get_mailbox_uidnext）：
--   INSERT INTO mailbox_uidnext (mailbox_id, uidnext)
--   SELECT ?, COALESCE(MAX(mm.mail_id),0)+1 FROM mail_mailbox mm WHERE mm.mailbox_id = ?
--   ON DUPLICATE KEY UPDATE
--     uidnext = GREATEST(uidnext, (SELECT COALESCE(MAX(mm.mail_id),0)+1 FROM mail_mailbox mm WHERE mm.mailbox_id = ?));
-- 行锁串行化并发 + GREATEST 保证单调不回退。

CREATE TABLE IF NOT EXISTS mailbox_uidnext (
  mailbox_id BIGINT UNSIGNED PRIMARY KEY,
  uidnext    BIGINT UNSIGNED NOT NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
