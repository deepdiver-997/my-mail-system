-- mails.subject 255 → 998（2026-09-04 入站丢信事故）
--
-- 事故：入站邮件原始 Subject 头（含 MIME encoded-word 展开后）超过 255 字符，
-- INSERT INTO mails 报 errno 1406 (Data too long) → 持久化事务整体回滚，
-- cleanup_failed_mail 连同已落盘正文一并删除 → 邮件静默丢失（发件方已收 250）。
--
-- RFC 5322 单行上限 998 字节，encoded-word 展开后的原始主题基本都落在此量级。
-- 应用层兜底：db::sql::clip_subject_for_db 按 escape 后字符预算截断（同 998），
-- 覆盖 SMTP 入站 / FSM DATA_END / IMAP APPEND / 入站去重 WHERE 全部路径。

ALTER TABLE mails
  MODIFY COLUMN subject VARCHAR(998) NOT NULL COMMENT '邮件主题（超长截断，≤998 字符）';
