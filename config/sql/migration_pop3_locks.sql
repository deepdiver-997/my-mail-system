-- pop3_session_lock — RFC 1939 §6 mailbox lock
-- 同账号单会话：DELE→QUIT 提交期间，阻止第二个 POP3 会话进入
-- acquire = INSERT ... ON DUPLICATE KEY UPDATE（详见 traditional_pop3_fsm.tpp）
-- release = DELETE WHERE user_id = ? AND session_id = ?
-- sweeper: DELETE WHERE last_heartbeat < NOW() - INTERVAL 5 MINUTE（v1 暂未实现）

CREATE TABLE IF NOT EXISTS pop3_session_lock (
  user_id        BIGINT UNSIGNED PRIMARY KEY,
  session_id     VARCHAR(64)     NOT NULL,
  acquired_at    DATETIME        NOT NULL,
  last_heartbeat DATETIME        NOT NULL,
  INDEX idx_heartbeat (last_heartbeat)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
