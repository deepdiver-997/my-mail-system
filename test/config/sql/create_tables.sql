-- 创建数据库
CREATE DATABASE IF NOT EXISTS mail;

-- 使用数据库
USE mail;

-- 创建用户表
CREATE TABLE IF NOT EXISTS users (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    mail_address VARCHAR(255) NOT NULL UNIQUE COMMENT '邮箱地址，必须唯一',
    password VARCHAR(255) NOT NULL COMMENT '加密存储的密码',
    name VARCHAR(100) NOT NULL COMMENT '用户名称',
    telephone VARCHAR(20) COMMENT '电话号码',
    status TINYINT NOT NULL DEFAULT 1 COMMENT '账户状态: 1=正常, 0=禁用',
    last_login_time TIMESTAMP NULL DEFAULT NULL COMMENT '最后登录时间',
    register_time TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT '注册时间',
    INDEX idx_mail_address (mail_address)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='用户信息表';

-- 创建邮件表（只存储邮件元数据，不存储收发件人）
CREATE TABLE IF NOT EXISTS mails (
    id BIGINT PRIMARY KEY NOT NULL COMMENT '邮件ID（由服务器生成的Snowflake ID）',
    subject VARCHAR(255) NOT NULL COMMENT '邮件主题',
    body_path VARCHAR(512) COMMENT '邮件正文文件路径',
    send_time TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT '发送时间',
    spam_status TINYINT NOT NULL DEFAULT 0 COMMENT '0=未检测 1=正常 2=垃圾 3=检测中',
    spam_score FLOAT DEFAULT NULL COMMENT '垃圾概率分数（LLM 模式）',
    INDEX idx_send_time (send_time),
    INDEX idx_spam_status (spam_status)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='邮件元数据表';

-- 创建邮件收发件人关系表（一份邮件可能有多个收件人）
CREATE TABLE IF NOT EXISTS mail_recipients (
    id BIGINT PRIMARY KEY NOT NULL COMMENT '关系记录ID（由服务器生成的Snowflake ID）',
    mail_id BIGINT NOT NULL COMMENT '邮件ID',
    sender VARCHAR(255) NOT NULL COMMENT '发件人邮箱地址',
    recipient VARCHAR(255) NOT NULL COMMENT '收件人邮箱地址',
    source_message_id VARCHAR(255) NULL COMMENT '上游 Message-ID，用于入站去重',
    status INT NOT NULL DEFAULT 0 COMMENT '邮件状态：0已读，1未读，2未送达，3草稿，4垃圾邮件，5已删除',
    send_time TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT '记录创建时间',
    FOREIGN KEY (mail_id) REFERENCES mails(id) ON DELETE CASCADE,
    UNIQUE KEY uk_mail_sender_recipient (mail_id, sender, recipient) COMMENT '同一份邮件同一对收发件人只记录一次',
    INDEX idx_mail_id (mail_id),
    INDEX idx_sender (sender),
    INDEX idx_recipient (recipient),
    INDEX idx_sender_recipient_msgid (sender, recipient, source_message_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='邮件收发件人关系表';

-- 创建出站投递队列表
CREATE TABLE IF NOT EXISTS mail_outbox (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    mail_id BIGINT NOT NULL COMMENT '邮件ID',
    sender VARCHAR(255) NOT NULL COMMENT '发件人邮箱地址',
    recipient VARCHAR(255) NOT NULL COMMENT '外部收件人邮箱地址',
    status TINYINT NOT NULL DEFAULT 0 COMMENT '0-PENDING 1-SENDING 2-SENT 3-RETRY 4-DEAD',
    priority TINYINT NOT NULL DEFAULT 0 COMMENT '优先级',
    attempt_count INT NOT NULL DEFAULT 0 COMMENT '尝试次数',
    max_attempts INT NOT NULL DEFAULT 8 COMMENT '最大重试次数',
    next_attempt_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT '下次可投递时间',
    lease_owner VARCHAR(128) NULL COMMENT '租约持有者',
    lease_until TIMESTAMP NULL COMMENT '租约过期时间',
    last_error_code VARCHAR(64) NULL COMMENT '最近错误码',
    last_error_message TEXT NULL COMMENT '最近错误信息',
    smtp_response TEXT NULL COMMENT '远端 SMTP 响应',
    sent_at TIMESTAMP NULL COMMENT '成功投递时间',
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    INDEX idx_status_attempt (status, next_attempt_at, priority),
    INDEX idx_lease_until (lease_until),
    INDEX idx_mail_id (mail_id),
    CONSTRAINT fk_outbox_mail FOREIGN KEY (mail_id) REFERENCES mails(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='SMTP 出站投递状态表';

-- 创建附件表
CREATE TABLE IF NOT EXISTS attachments (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    mail_id BIGINT NOT NULL COMMENT '所属邮件ID',
    filename VARCHAR(255) NOT NULL COMMENT '文件名',
    filepath VARCHAR(255) NOT NULL COMMENT '文件存储路径',
    file_size BIGINT NOT NULL COMMENT '文件大小（字节）',
    mime_type VARCHAR(100) COMMENT '文件MIME类型',
    upload_time TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT '上传时间',
    FOREIGN KEY (mail_id) REFERENCES mails(id) ON DELETE CASCADE,
    INDEX idx_mail_id (mail_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='邮件附件表';

-- 创建邮箱表
CREATE TABLE IF NOT EXISTS mailboxes (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    user_id BIGINT NOT NULL COMMENT '所属用户ID',
    name VARCHAR(100) NOT NULL COMMENT '邮箱名称',
    is_system BOOLEAN NOT NULL DEFAULT FALSE COMMENT '是否为系统默认邮箱',
    box_type INT COMMENT '系统邮箱类型：1收件箱，2发件箱，3垃圾箱，4草稿箱，5已删除',
    create_time TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT '创建时间',
    FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE,
    UNIQUE KEY uk_user_box (user_id, name),
    INDEX idx_user_id (user_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='用户邮箱表';

-- 创建邮件-邮箱关联表
CREATE TABLE IF NOT EXISTS mail_mailbox (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    mail_id BIGINT NOT NULL COMMENT '邮件ID',
    mailbox_id BIGINT NOT NULL COMMENT '邮箱ID',
    user_id BIGINT NOT NULL COMMENT '用户ID',
    is_starred BOOLEAN NOT NULL DEFAULT FALSE COMMENT '是否标星',
    is_important BOOLEAN NOT NULL DEFAULT FALSE COMMENT '是否重要',
    is_deleted BOOLEAN NOT NULL DEFAULT FALSE COMMENT '是否已删除',
    add_time TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT '添加时间',
    FOREIGN KEY (mail_id) REFERENCES mails(id) ON DELETE CASCADE,
    FOREIGN KEY (mailbox_id) REFERENCES mailboxes(id) ON DELETE CASCADE,
    FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE,
    UNIQUE KEY uk_mail_box_user (mail_id, mailbox_id, user_id),
    INDEX idx_mail_id (mail_id),
    INDEX idx_mailbox_id (mailbox_id),
    INDEX idx_user_id (user_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='邮件-邮箱关联表';

-- 创建分片映射表（table 模式分片路由使用）
CREATE TABLE IF NOT EXISTS user_shards (
    id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    email VARCHAR(320) NOT NULL UNIQUE COMMENT '用户邮箱',
    shard_id INT UNSIGNED NOT NULL COMMENT '分片索引',
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    INDEX idx_email (email)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='用户-分片映射表';

-- 创建系统默认邮箱的存储过程
DROP PROCEDURE IF EXISTS create_default_mailboxes;
DELIMITER //
CREATE PROCEDURE create_default_mailboxes(IN p_user_id BIGINT)
BEGIN
    -- 创建收件箱
    INSERT INTO mailboxes (user_id, name, is_system, box_type) 
    VALUES (p_user_id, '收件箱', TRUE, 1);
    
    -- 创建发件箱
    INSERT INTO mailboxes (user_id, name, is_system, box_type) 
    VALUES (p_user_id, '发件箱', TRUE, 2);
    
    -- 创建垃圾箱
    INSERT INTO mailboxes (user_id, name, is_system, box_type) 
    VALUES (p_user_id, '垃圾箱', TRUE, 3);
    
    -- 创建已删除
    INSERT INTO mailboxes (user_id, name, is_system, box_type) 
    VALUES (p_user_id, '已删除', TRUE, 4);
    
    -- 创建草稿箱
    INSERT INTO mailboxes (user_id, name, is_system, box_type) 
    VALUES (p_user_id, '草稿箱', TRUE, 5);
END //
DELIMITER ;

-- 创建用户注册后自动创建默认邮箱的触发器
DROP TRIGGER IF EXISTS after_user_insert;
DELIMITER //
CREATE TRIGGER after_user_insert
AFTER INSERT ON users
FOR EACH ROW
BEGIN
    CALL create_default_mailboxes(NEW.id);
END //
DELIMITER ;

-- ============================================================
-- 迁移：已有数据库加垃圾检测字段
-- ============================================================
-- 新版本 mails 表增加了 spam_status 和 spam_score 列。
-- 对已有数据库执行：
--   ALTER TABLE mails ADD COLUMN spam_status TINYINT NOT NULL DEFAULT 0;
--   ALTER TABLE mails ADD COLUMN spam_score FLOAT DEFAULT NULL;
--   ALTER TABLE mails ADD INDEX idx_spam_status (spam_status);