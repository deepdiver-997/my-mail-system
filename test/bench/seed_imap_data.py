#!/usr/bin/env python3
"""IMAP 读路径压测的 seed 数据：给测试用户 INBOX 灌 K 封邮件（DB 行 + body 文件）。

读场景不产生新数据，seed 一次即可；重复跑只是重读同一份数据。
用 INSERT IGNORE（按 mail_id 幂等），重复执行不会重复灌。
注意：读路径的唯一写入是 get_mailbox_uidnext 的高水位推进（幂等 GREATEST），
对压测数据无害。

用法: python3 test/bench/seed_imap_data.py [--mails 200] [--size 2048]
"""
import argparse, os, subprocess, sys

DB = ["mysql", "-h", "localhost", "-u", "mail_test", "-pabjskKA09qjf", "mail"]

def q(sql):
    r = subprocess.run(DB + ["-e", sql], capture_output=True, text=True)
    if r.returncode != 0:
        print("SQL error:", r.stderr, file=sys.stderr)
        sys.exit(1)

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--mails", type=int, default=200, help="每用户 INBOX 邮件数")
    p.add_argument("--user", default="test2@scut.email")
    p.add_argument("--mailbox", type=int, default=6, help="收件箱 mailbox id（默认 test2 的）")
    p.add_argument("--body-dir", default="mail", help="body 文件目录（相对项目根）")
    p.add_argument("--size", type=int, default=2048, help="每封 body 字节数")
    a = p.parse_args()

    r = subprocess.run(DB + ["-N", "-e", f"SELECT id FROM users WHERE mail_address='{a.user}'"],
                       capture_output=True, text=True)
    uid = r.stdout.strip()
    if not uid:
        print(f"user {a.user} not found", file=sys.stderr); sys.exit(1)
    uid = int(uid)

    os.makedirs(a.body_dir, exist_ok=True)
    base = 10**18          # mails.id（单调递增，uidnext 友好）
    rbase = 2 * 10**18     # mail_recipients.id（服务端生成式）

    m_rows, r_rows, mm_rows = [], [], []
    for i in range(a.mails):
        mid = base + i
        subject = f"Bench mail {i}"
        body_path = os.path.abspath(os.path.join(a.body_dir, str(mid)))
        m_rows.append(f"({mid}, '{subject}', '{body_path}', NOW())")
        r_rows.append(f"({rbase+i}, {mid}, 'sender@scut.email', '{a.user}', 0)")
        # 收件人 status=0 已读；改 1 可测 UNSEEN 路径
        mm_rows.append(f"({mid}, {a.mailbox}, {uid}, 0, 0, 0, NOW())")
        with open(body_path, "wb") as f:
            f.write((f"Subject: {subject}\r\n\r\n" + ("x" * a.size)).encode())

    q("INSERT IGNORE INTO mails (id, subject, body_path, send_time) VALUES " + ",".join(m_rows))
    q("INSERT IGNORE INTO mail_recipients (id, mail_id, sender, recipient, status) VALUES " + ",".join(r_rows))
    q("INSERT IGNORE INTO mail_mailbox (mail_id, mailbox_id, user_id, is_starred, is_important, is_deleted, add_time) VALUES " + ",".join(mm_rows))
    print(f"seeded {a.mails} mails for {a.user} (mailbox {a.mailbox}), bodies in {a.body_dir}/")

if __name__ == "__main__":
    main()
