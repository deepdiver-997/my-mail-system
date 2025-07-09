-- 创建用户表
CREATE TABLE IF NOT EXISTS users (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    mail_address VARCHAR(255) NOT NULL UNIQUE COMMENT '邮箱地址，必须唯一',
    password VARCHAR(255) NOT NULL COMMENT '加密存储的密码',
    name VARCHAR(100) NOT NULL COMMENT '用户名称',
    telephone VARCHAR(20) COMMENT '电话号码',
    register_time TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT '注册时间',
    INDEX idx_mail_address (mail_address)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='用户信息表';

-- 创建邮件表
CREATE TABLE IF NOT EXISTS mails (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    sender VARCHAR(255) NOT NULL COMMENT '发件人邮箱地址',
    recipient VARCHAR(255) NOT NULL COMMENT '收件人邮箱地址',
    subject VARCHAR(255) NOT NULL COMMENT '邮件主题',
    body TEXT COMMENT '邮件正文',
    send_time TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT '发送时间',
    is_draft BOOLEAN NOT NULL DEFAULT FALSE COMMENT '是否为草稿',
    is_read BOOLEAN NOT NULL DEFAULT FALSE COMMENT '是否已读',
    INDEX idx_sender (sender),
    INDEX idx_recipient (recipient),
    INDEX idx_send_time (send_time)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='邮件信息表';

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
    box_type INT COMMENT '系统邮箱类型：1收件箱，2发件箱，3垃圾箱，4已删除',
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

-- 创建系统默认邮箱的存储过程
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
DELIMITER //
CREATE TRIGGER after_user_insert
AFTER INSERT ON users
FOR EACH ROW
BEGIN
    CALL create_default_mailboxes(NEW.id);
END //
DELIMITER ;