#!/usr/bin/env python3
"""
用户注册工具 —— 替代 tools/register_user.cpp

用法:
  python3 scripts/register_user.py <db_config.json> <email> <password> <name> [phone]

示例:
  python3 scripts/register_user.py config/db_config.json test@example.com <PASSWORD> "Test User"

依赖:
  pip install bcrypt mysql-connector-python
"""

import json
import sys
import re

def load_db_config(path: str) -> dict:
    with open(path, 'r') as f:
        return json.load(f)

def bcrypt_hash(password: str) -> str:
    """Generate bcrypt hash of password."""
    try:
        import bcrypt
        return bcrypt.hashpw(password.encode('utf-8'), bcrypt.gensalt()).decode('utf-8')
    except ImportError:
        print("错误: 需要安装 bcrypt 库: pip install bcrypt")
        sys.exit(1)

def main():
    if len(sys.argv) < 5:
        print(f"用法: {sys.argv[0]} <db_config.json> <email> <password> <name> [phone]")
        print(f"\n示例:")
        print(f"  {sys.argv[0]} config/db_config.json test@example.com <PASSWORD> 'Test User'")
        sys.exit(1)

    db_config_path = sys.argv[1]
    email = sys.argv[2]
    password = sys.argv[3]
    name = sys.argv[4]
    phone = sys.argv[5] if len(sys.argv) > 5 else ""

    # 基本校验
    if not re.match(r'^[^@]+@[^@]+\.[^@]+$', email):
        print(f"错误: 邮箱格式无效: {email}")
        sys.exit(1)

    if not password or len(password) < 4:
        print("错误: 密码至少 4 个字符")
        sys.exit(1)

    if not name:
        print("错误: 姓名不能为空")
        sys.exit(1)

    # 加载数据库配置
    try:
        db_cfg = load_db_config(db_config_path)
    except Exception as e:
        print(f"错误: 无法读取数据库配置 {db_config_path}: {e}")
        sys.exit(1)

    # bcrypt 哈希
    hashed = bcrypt_hash(password)

    # 连接 MySQL
    try:
        import mysql.connector
    except ImportError:
        print("错误: 需要安装 mysql-connector-python: pip install mysql-connector-python")
        sys.exit(1)

    try:
        conn = mysql.connector.connect(
            host=db_cfg.get("host", "127.0.0.1"),
            port=int(db_cfg.get("port", 3306)),
            user=db_cfg.get("user", "root"),
            password=db_cfg.get("password", ""),
            database=db_cfg.get("database", "mail"),
            charset='utf8mb4',
        )
    except Exception as e:
        print(f"错误: 数据库连接失败: {e}")
        sys.exit(1)

    cursor = conn.cursor()

    # 检查是否已存在
    cursor.execute("SELECT id FROM users WHERE mail_address = %s", (email,))
    if cursor.fetchone():
        print(f"错误: 邮箱已注册: {email}")
        cursor.close()
        conn.close()
        sys.exit(1)

    # 插入用户
    try:
        if phone:
            cursor.execute(
                "INSERT INTO users (mail_address, password, name, telephone) VALUES (%s, %s, %s, %s)",
                (email, hashed, name, phone)
            )
        else:
            cursor.execute(
                "INSERT INTO users (mail_address, password, name) VALUES (%s, %s, %s)",
                (email, hashed, name)
            )
        conn.commit()

        # 获取新用户 ID
        cursor.execute("SELECT LAST_INSERT_ID()")
        user_id = cursor.fetchone()[0]

        # 检查触发器是否已创建默认邮箱
        cursor.execute("SELECT COUNT(*) FROM mailboxes WHERE user_id = %s", (user_id,))
        mailbox_count = cursor.fetchone()[0]

        print("用户注册成功!")
        print(f"  邮箱:    {email}")
        print(f"  姓名:    {name}")
        print(f"  用户ID:  {user_id}")
        print(f"  bcrypt:  {hashed[:7]}...")
        if mailbox_count > 0:
            print(f"  (触发器已自动创建 {mailbox_count} 个默认邮箱: 收件箱/发件箱/垃圾箱/已删除/草稿箱)")
        else:
            print("  (注意: 触发器未自动创建默认邮箱，请检查数据库)")

    except Exception as e:
        conn.rollback()
        print(f"错误: 插入用户失败: {e}")
        sys.exit(1)
    finally:
        cursor.close()
        conn.close()


if __name__ == '__main__':
    main()
