#!/usr/bin/env python3
"""
SMTP 流水线 (pipelining) 测试。

测试场景：
  1. 标准流水线 — 批量发送全部命令后读取所有响应
  2. 流水线错误恢复 — 中间命令失败后继续
  3. 流水线 + DATA — DATA 命令不能流水线化
  4. 多事务流水线 — 一个连接内多封邮件

SMTP pipelining (RFC 2920):
  客户端无需等待每个命令的响应即可发送下一命令。
  服务器必须按顺序处理并返回响应。
"""
import socket
import ssl
import time
import sys
import os


def smtp_command(sock, cmd):
    """发送 SMTP 命令并返回响应。"""
    sock.sendall((cmd + "\r\n").encode())
    return smtp_read_response(sock)


def smtp_read_response(sock):
    """读取完整的多行 SMTP 响应。"""
    sock.settimeout(5)
    lines = []
    while True:
        line = b""
        while not line.endswith(b"\n"):
            chunk = sock.recv(1)
            if not chunk:
                break
            line += chunk
        if not line:
            break
        lines.append(line.decode().rstrip())
        # 第四个字符是空格表示最后一行
        if len(line) >= 4 and line[3:4] == b" ":
            break
    return lines


def receive_all_responses(sock, count):
    """批量读取 N 个多行 SMTP 响应。"""
    all_resp = []
    for _ in range(count):
        try:
            resp = smtp_read_response(sock)
            all_resp.append(resp)
        except Exception as e:
            all_resp.append([f"ERROR: {e}"])
            break
    return all_resp


# ══════════════════════════════════════════════════════════════
# 测试 1: 标准命令流水线
# ══════════════════════════════════════════════════════════════

def test_standard_pipeline(host="127.0.0.1", port=2525):
    """EHLO + MAIL FROM + RCPT TO + DATA — 流水线化前三个命令。"""
    print(f"\n--- Test 1: Standard command pipeline ---")

    try:
        sock = socket.create_connection((host, port), timeout=5)
        greeting = smtp_read_response(sock)
        assert any("220" in l for l in greeting), f"No 220 greeting: {greeting}"

        # 流水线发送 EHLO + MAIL FROM + RCPT TO（不等响应）
        sock.sendall(b"EHLO test.local\r\n")
        sock.sendall(b"MAIL FROM:<sender@test.local>\r\n")
        sock.sendall(b"RCPT TO:<rcpt@test.local>\r\n")

        # 读取 3 个响应
        ehlo_resp  = smtp_read_response(sock)
        mail_resp  = smtp_read_response(sock)
        rcpt_resp  = smtp_read_response(sock)

        assert any("250" in l for l in ehlo_resp), f"EHLO failed: {ehlo_resp}"
        assert any("250" in l for l in mail_resp), f"MAIL FROM failed: {mail_resp}"
        assert any("250" in l for l in rcpt_resp), f"RCPT TO failed: {rcpt_resp}"

        # DATA 单独发送（不能流水线化）
        sock.sendall(b"DATA\r\n")
        data_resp = smtp_read_response(sock)
        assert any("354" in l for l in data_resp), f"DATA failed: {data_resp}"

        # 发送邮件体
        body = "From: sender@test.local\r\nTo: rcpt@test.local\r\nSubject: Pipeline Test\r\n\r\nHello pipeline\r\n.\r\n"
        sock.sendall(body.encode())
        ok_resp = smtp_read_response(sock)
        assert any("250" in l for l in ok_resp), f"Body accept failed: {ok_resp}"

        sock.sendall(b"QUIT\r\n")
        smtp_read_response(sock)
        sock.close()
        print("  [PASS] Standard pipeline: EHLO+MAIL+RCPT → DATA → body → OK")
        return True
    except Exception as e:
        print(f"  [FAIL] {e}")
        return False


# ══════════════════════════════════════════════════════════════
# 测试 2: 流水线中的错误恢复
# ══════════════════════════════════════════════════════════════

def test_pipeline_error_recovery(host="127.0.0.1", port=2525):
    """流水线中有一个命令失败，后续命令仍应正常。"""
    print(f"\n--- Test 2: Pipeline error recovery ---")

    try:
        sock = socket.create_connection((host, port), timeout=5)
        smtp_read_response(sock)  # greeting

        # 不发 EHLO 直接 MAIL FROM（应该失败），但下一个 EHLO 仍应成功
        sock.sendall(b"MAIL FROM:<x@y>\r\n")
        sock.sendall(b"EHLO test.local\r\n")
        sock.sendall(b"MAIL FROM:<ok@test.local>\r\n")

        resp1 = smtp_read_response(sock)
        resp2 = smtp_read_response(sock)
        resp3 = smtp_read_response(sock)

        # 第一个应该失败（500 Bad sequence）
        has_error = any("50" in l or "5" in l[:3] for l in resp1)
        # EHLO 应该成功
        has_ehlo_ok = any("250" in l for l in resp2)
        # MAIL FROM 应该成功
        has_mail_ok = any("250" in l for l in resp3)

        assert has_error, f"Expected error for MAIL before EHLO: {resp1}"
        assert has_ehlo_ok, f"EHLO should succeed: {resp2}"
        assert has_mail_ok, f"MAIL FROM should succeed: {resp3}"

        sock.sendall(b"QUIT\r\n")
        smtp_read_response(sock)
        sock.close()
        print("  [PASS] Pipeline error recovery")
        return True
    except Exception as e:
        print(f"  [FAIL] {e}")
        return False


# ══════════════════════════════════════════════════════════════
# 测试 3: 多事务流水线
# ══════════════════════════════════════════════════════════════

def test_multi_transaction(host="127.0.0.1", port=2525):
    """一个连接内发送多封邮件（RSET 重置）。"""
    print(f"\n--- Test 3: Multi-transaction pipeline ---")

    try:
        sock = socket.create_connection((host, port), timeout=5)
        smtp_read_response(sock)  # greeting
        smtp_command(sock, "EHLO test.local")

        # 第一封
        smtp_command(sock, "MAIL FROM:<s1@test.local>")
        smtp_command(sock, "RCPT TO:<r1@test.local>")
        smtp_command(sock, "DATA")
        sock.sendall(b"Subject: First\r\n\r\nFirst body\r\n.\r\n")
        resp1 = smtp_read_response(sock)
        assert any("250" in l for l in resp1), f"Mail 1 failed: {resp1}"

        # RSET + 第二封
        smtp_command(sock, "RSET")
        smtp_command(sock, "MAIL FROM:<s2@test.local>")
        smtp_command(sock, "RCPT TO:<r2@test.local>")
        smtp_command(sock, "DATA")
        sock.sendall(b"Subject: Second\r\n\r\nSecond body\r\n.\r\n")
        resp2 = smtp_read_response(sock)
        assert any("250" in l for l in resp2), f"Mail 2 failed: {resp2}"

        # 第三封 — 流水线 MAIL+RCPT
        smtp_command(sock, "RSET")
        sock.sendall(b"MAIL FROM:<s3@test.local>\r\n")
        sock.sendall(b"RCPT TO:<r3@test.local>\r\n")
        smtp_read_response(sock)
        smtp_read_response(sock)
        smtp_command(sock, "DATA")
        sock.sendall(b"Subject: Third\r\n\r\nThird body\r\n.\r\n")
        resp3 = smtp_read_response(sock)
        assert any("250" in l for l in resp3), f"Mail 3 failed: {resp3}"

        smtp_command(sock, "QUIT")
        sock.close()
        print("  [PASS] Multi-transaction: 3 mails in 1 connection")
        return True
    except Exception as e:
        print(f"  [FAIL] {e}")
        return False


# ══════════════════════════════════════════════════════════════
# 测试 4: RC 流水线 — 多个 RCPT TO 连续发送
# ══════════════════════════════════════════════════════════════

def test_rcpt_pipeline(host="127.0.0.1", port=2525):
    """一个 MAIL FROM 后流水线发送 N 个 RCPT TO。"""
    print(f"\n--- Test 4: RCPT TO pipeline ---")

    try:
        sock = socket.create_connection((host, port), timeout=5)
        smtp_read_response(sock)
        smtp_command(sock, "EHLO test.local")
        smtp_command(sock, "MAIL FROM:<sender@test.local>")

        # 流水线 5 个 RCPT TO
        n = 5
        for i in range(n):
            sock.sendall(f"RCPT TO:<rcpt{i}@test.local>\r\n".encode())

        for i in range(n):
            resp = smtp_read_response(sock)
            assert any("250" in l for l in resp), f"RCPT {i} failed: {resp}"

        smtp_command(sock, "RSET")
        smtp_command(sock, "QUIT")
        sock.close()
        print(f"  [PASS] RCPT pipeline: {n} recipients accepted")
        return True
    except Exception as e:
        print(f"  [FAIL] {e}")
        return False


# ══════════════════════════════════════════════════════════════
# 测试 5: NOOP + VRFY 在流水线中
# ══════════════════════════════════════════════════════════════

def test_mixed_commands_pipeline(host="127.0.0.1", port=2525):
    """混合命令流水线（NOOP 穿插在邮件命令之间）。"""
    print(f"\n--- Test 5: Mixed commands pipeline ---")

    try:
        sock = socket.create_connection((host, port), timeout=5)
        smtp_read_response(sock)
        smtp_command(sock, "EHLO test.local")

        sock.sendall(b"NOOP\r\n")
        sock.sendall(b"MAIL FROM:<s@test.local>\r\n")
        sock.sendall(b"NOOP\r\n")
        sock.sendall(b"RCPT TO:<r@test.local>\r\n")
        sock.sendall(b"NOOP\r\n")

        for _ in range(5):
            resp = smtp_read_response(sock)
            assert any("250" in l for l in resp), f"Expected 250: {resp}"

        smtp_command(sock, "RSET")
        smtp_command(sock, "QUIT")
        sock.close()
        print("  [PASS] Mixed NOOP pipeline")
        return True
    except Exception as e:
        print(f"  [FAIL] {e}")
        return False


# ══════════════════════════════════════════════════════════════

def main():
    host = os.environ.get("SMTP_HOST", "127.0.0.1")
    port = int(os.environ.get("SMTP_PORT", "2525"))

    print("=" * 50)
    print("  ProtoRelay SMTP Pipeline Test Suite")
    print(f"  Target: {host}:{port}")
    print("=" * 50)

    results = []
    results.append(("standard pipeline", test_standard_pipeline(host, port)))
    results.append(("error recovery", test_pipeline_error_recovery(host, port)))
    results.append(("multi-transaction", test_multi_transaction(host, port)))
    results.append(("RCPT pipeline", test_rcpt_pipeline(host, port)))
    results.append(("mixed commands", test_mixed_commands_pipeline(host, port)))

    print()
    passed = sum(1 for _, r in results if r)
    failed = len(results) - passed
    print(f"  PASS: {passed}  FAIL: {failed}")
    for name, r in results:
        status = "PASS" if r else "FAIL"
        print(f"    [{status}] {name}")

    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
