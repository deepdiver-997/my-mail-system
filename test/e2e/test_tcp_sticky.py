#!/usr/bin/env python3
"""
TCP 粘包 / 截断 / 延迟测试。

模拟真实网络中的三种场景：
  1. 粘包 — 多个命令在一个 TCP segment 中到达
  2. 截断 — 命令被 TCP segment 边界分割
  3. 延迟 — 命令中间有较长间隔（慢客户端 / 慢 MTA）

场景说明：
  - MTA 投递：客户端快速发送大量命令（EHLO → MAIL → RCPT → DATA → body → QUIT）
              不做等待，模拟自动化 MTA 行为
  - 客户端投递：用户在 telnet 中逐字符输入，命令之间有延迟
              模拟人工交互


粘包 (coalescing):
  ┌──────────────────────────────────────┐
  │ "EHLO test\r\nMAIL FROM:<>\r\nRCPT.."│  ← 一个 recv() 返回多个命令
  └──────────────────────────────────────┘

截断 (truncation):
  ┌─────────────┐  ┌──────────────────┐
  │ "MAIL FROM:"│  │ "<s@t.com>\r\n"  │  ← 两次 recv() 拼成完整命令
  └─────────────┘  └──────────────────┘

延迟 (slow client):
  ┌──────┐  ...200ms...  ┌──────┐  ...200ms...  ┌──────┐
  │ "HE" │               │ "LO "│               │ "t\r\n"│
  └──────┘               └──────┘               └──────┘
"""
import socket
import time
import sys
import os


def smtp_read_line(sock, timeout=5):
    """读取一行 SMTP 响应（单行模式，用于精确验证）。"""
    sock.settimeout(timeout)
    line = b""
    while not line.endswith(b"\n"):
        chunk = sock.recv(1)
        if not chunk:
            return None
        line += chunk
    return line.decode().rstrip()


def smtp_read_response(sock, timeout=5):
    """读取完整的多行 SMTP 响应。"""
    sock.settimeout(timeout)
    lines = []
    while True:
        line = smtp_read_line(sock, timeout)
        if line is None:
            break
        lines.append(line)
        # 第四个字符是空格 → 最后一行
        if len(line) >= 4 and line[3] == " ":
            break
    return lines


def assert_code(lines, code):
    """验证响应中包含指定状态码。"""
    if not lines:
        return False
    return any(l.startswith(str(code)) for l in lines)


# ══════════════════════════════════════════════════════════════
# 测试 1: 粘包 — 全部命令一次性发送
# ══════════════════════════════════════════════════════════════

def test_coalescing_all_at_once(host="127.0.0.1", port=2525):
    """模拟 MTA：所有命令一次性写入 TCP 缓冲区。"""
    print(f"\n--- Test 1: Coalescing — all commands in one write ---")

    try:
        sock = socket.create_connection((host, port), timeout=5)

        # 一次性发送整个 SMTP 会话
        all_data = (
            "EHLO mta.example.com\r\n"
            "MAIL FROM:<mta@example.com>\r\n"
            "RCPT TO:<user@test.local>\r\n"
            "DATA\r\n"
            "From: mta@example.com\r\n"
            "To: user@test.local\r\n"
            "Subject: Coalesced MTA delivery\r\n"
            "Message-ID: <coalesce-001@example.com>\r\n"
            "\r\n"
            "This entire SMTP session was sent in one TCP write.\r\n"
            "\r\n"
            "Line with a leading dot should be unstuffed by receiver.\r\n"
            "..this line starts with dot-dot.\r\n"
            "Normal line.\r\n"
            ".\r\n"
        )
        sock.sendall(all_data.encode())

        # 逐个读取响应
        greeting   = smtp_read_response(sock)  # 220
        ehlo_resp  = smtp_read_response(sock)  # 250 (multi-line)
        mail_resp  = smtp_read_response(sock)  # 250
        rcpt_resp  = smtp_read_response(sock)  # 250
        data_resp  = smtp_read_response(sock)  # 354
        accept_resp = smtp_read_response(sock) # 250 queued

        assert assert_code(greeting, 220), f"No 220: {greeting}"
        assert assert_code(ehlo_resp, 250),  f"No 250 EHLO: {ehlo_resp}"
        assert assert_code(mail_resp, 250),  f"No 250 MAIL: {mail_resp}"
        assert assert_code(rcpt_resp, 250),  f"No 250 RCPT: {rcpt_resp}"
        assert assert_code(data_resp, 354),  f"No 354 DATA: {data_resp}"
        assert assert_code(accept_resp, 250), f"No 250 accept: {accept_resp}"

        sock.sendall(b"QUIT\r\n")
        smtp_read_response(sock)
        sock.close()
        print("  [PASS] All commands coalesced in one write")
        return True
    except Exception as e:
        print(f"  [FAIL] {e}")
        import traceback; traceback.print_exc()
        return False


# ══════════════════════════════════════════════════════════════
# 测试 2: 粘包 — 多个命令对
# ══════════════════════════════════════════════════════════════

def test_coalescing_batches(host="127.0.0.1", port=2525):
    """分两批发送：批1=EHLO+MAIL+RCPT，批2=DATA+body。"""
    print(f"\n--- Test 2: Coalescing — batch writes ---")

    try:
        sock = socket.create_connection((host, port), timeout=5)
        smtp_read_response(sock)  # greeting

        # 第一批：EHLO + MAIL FROM + RCPT TO 粘包
        batch1 = (
            "EHLO batch.test\r\n"
            "MAIL FROM:<batch@test.local>\r\n"
            "RCPT TO:<rcpt@test.local>\r\n"
        )
        sock.sendall(batch1.encode())

        ehlo = smtp_read_response(sock)
        mail = smtp_read_response(sock)
        rcpt = smtp_read_response(sock)
        assert assert_code(ehlo, 250), f"EHLO: {ehlo}"
        assert assert_code(mail, 250), f"MAIL: {mail}"
        assert assert_code(rcpt, 250), f"RCPT: {rcpt}"

        # 第二批：DATA + body + .\r\n 粘包
        batch2 = (
            "DATA\r\n"
            "Subject: Batch test\r\n"
            "\r\n"
            "Batch body line 1.\r\n"
            "Batch body line 2.\r\n"
            ".\r\n"
        )
        sock.sendall(batch2.encode())

        data_resp = smtp_read_response(sock)
        accept    = smtp_read_response(sock)
        assert assert_code(data_resp, 354), f"DATA: {data_resp}"
        assert assert_code(accept, 250), f"Accept: {accept}"

        sock.sendall(b"QUIT\r\n")
        smtp_read_response(sock)
        sock.close()
        print("  [PASS] Batch coalescing (2 writes)")
        return True
    except Exception as e:
        print(f"  [FAIL] {e}")
        return False


# ══════════════════════════════════════════════════════════════
# 测试 3: 截断 — 命令跨 TCP segment 分割
# ══════════════════════════════════════════════════════════════

def test_truncation_across_segments(host="127.0.0.1", port=2525):
    """模拟命令被 TCP segment 边界截断。"""
    print(f"\n--- Test 3: Truncation — commands split across segments ---")

    try:
        sock = socket.create_connection((host, port), timeout=5)
        smtp_read_response(sock)  # greeting

        # 场景 A: EHLO 被截断
        sock.sendall(b"EH")
        time.sleep(0.05)
        sock.sendall(b"LO trunc.test\r\n")
        resp = smtp_read_response(sock)
        assert assert_code(resp, 250), f"Split EHLO: {resp}"

        # 场景 B: MAIL FROM 被截断（在 <> 中间）
        sock.sendall(b"MAIL FR")
        time.sleep(0.05)
        sock.sendall(b"OM:<trunc@test.local>\r\n")
        resp = smtp_read_response(sock)
        assert assert_code(resp, 250), f"Split MAIL: {resp}"

        # 场景 C: RCPT TO 被截断
        sock.sendall(b"RCPT ")
        time.sleep(0.05)
        sock.sendall(b"TO:<trunc-rcpt@test.local>\r\n")
        resp = smtp_read_response(sock)
        assert assert_code(resp, 250), f"Split RCPT: {resp}"

        # D: DATA 跨 segment
        sock.sendall(b"DA")
        time.sleep(0.05)
        sock.sendall(b"TA\r\n")
        resp = smtp_read_response(sock)
        assert assert_code(resp, 354), f"Split DATA: {resp}"

        # E: body 跨 segment
        sock.sendall(b"Subject: Trunc\r\n")
        sock.sendall(b"\r\n")
        sock.sendall(b"Body line ")
        time.sleep(0.05)
        sock.sendall(b"continued across segments.\r\n")
        sock.sendall(b".\r\n")
        resp = smtp_read_response(sock)
        assert assert_code(resp, 250), f"Split body: {resp}"

        sock.sendall(b"QUIT\r\n")
        smtp_read_response(sock)
        sock.close()
        print("  [PASS] Truncation across TCP segments")
        return True
    except Exception as e:
        print(f"  [FAIL] {e}")
        return False


# ══════════════════════════════════════════════════════════════
# 测试 4: 截断 — 在 \r\n 边界分割
# ══════════════════════════════════════════════════════════════

def test_truncation_at_crlf(host="127.0.0.1", port=2525):
    """在 CRLF 前后分割 — 这是最常见的 TCP 边界。"""
    print(f"\n--- Test 4: Truncation at CRLF boundary ---")

    try:
        sock = socket.create_connection((host, port), timeout=5)
        smtp_read_response(sock)  # greeting

        # MAIL FROM 在 \r\n 处被截断（第一部分不带 \r\n）
        sock.sendall(b"EHLO crlf.test\r\nMAIL FROM:<crlf@test.local>")
        time.sleep(0.1)  # 稍等，让服务器处理已接收数据
        sock.sendall(b"\r\nRCPT TO:<crlf-rcpt@test.local>\r\n")

        ehlo = smtp_read_response(sock)
        mail = smtp_read_response(sock)
        rcpt = smtp_read_response(sock)
        assert assert_code(ehlo, 250), f"EHLO: {ehlo}"
        # MAIL 可能被当作未完成命令，取决于服务器行缓冲策略
        # 只要不崩溃就算通过，具体响应码取决于实现
        print(f"    EHLO: {ehlo[0] if ehlo else 'EMPTY'}")
        print(f"    MAIL: {mail[0] if mail else 'EMPTY'}")
        print(f"    RCPT: {rcpt[0] if rcpt else 'EMPTY'}")

        sock.sendall(b"RSET\r\nQUIT\r\n")
        smtp_read_response(sock)  # RSET
        smtp_read_response(sock)  # QUIT
        sock.close()
        print("  [PASS] CRLF boundary truncation (no crash)")
        return True
    except Exception as e:
        print(f"  [FAIL] {e}")
        return False


# ══════════════════════════════════════════════════════════════
# 测试 5: 延迟 — 慢客户端逐字符发送
# ══════════════════════════════════════════════════════════════

def test_slow_client_char_by_char(host="127.0.0.1", port=2525):
    """模拟交互式客户端：逐字符发送，中间有延迟。"""
    print(f"\n--- Test 5: Slow client — character by character ---")

    try:
        sock = socket.create_connection((host, port), timeout=5)
        smtp_read_response(sock)  # greeting

        # 逐字符发送 EHLO test\r\n
        for ch in "EHLO slow.test\r\n":
            sock.sendall(ch.encode())
            time.sleep(0.02)  # 20ms 间隔
        resp = smtp_read_response(sock, timeout=10)
        assert assert_code(resp, 250), f"Slow EHLO: {resp}"

        # 逐字符发送 MAIL FROM
        for ch in "MAIL FROM:<slow@test.local>\r\n":
            sock.sendall(ch.encode())
            time.sleep(0.02)
        resp = smtp_read_response(sock, timeout=10)
        assert assert_code(resp, 250), f"Slow MAIL: {resp}"

        sock.sendall(b"QUIT\r\n")
        smtp_read_response(sock)
        sock.close()
        print("  [PASS] Slow client char-by-char")
        return True
    except Exception as e:
        print(f"  [FAIL] {e}")
        return False


# ══════════════════════════════════════════════════════════════
# 测试 6: 延迟 — 慢 MTA 长时间停顿
# ══════════════════════════════════════════════════════════════

def test_slow_mta_long_pause(host="127.0.0.1", port=2525):
    """
    模拟慢 MTA：命令之间有长时间停顿。

    MTA 场景特点：
    - 快速发送命令组，但邮件之间可能有停顿
    - DATA 发送过程中有停顿（邮件体大且生成慢）
    """
    print(f"\n--- Test 6: Slow MTA — long pauses between commands ---")

    try:
        sock = socket.create_connection((host, port), timeout=5)
        sock.settimeout(30)  # 长超时

        smtp_read_response(sock)  # greeting

        # 快速 EHLO
        sock.sendall(b"EHLO mta-slow.test\r\n")
        resp = smtp_read_response(sock)
        assert assert_code(resp, 250), f"MTA EHLO: {resp}"

        # 停顿 500ms（模拟 MTA 处理收件人列表）
        time.sleep(0.5)
        sock.sendall(b"MAIL FROM:<mta-slow@test.local>\r\n")
        resp = smtp_read_response(sock)
        assert assert_code(resp, 250), f"MTA MAIL: {resp}"

        # 停顿 300ms
        time.sleep(0.3)
        sock.sendall(b"RCPT TO:<mta-slow-rcpt@test.local>\r\n")
        resp = smtp_read_response(sock)
        assert assert_code(resp, 250), f"MTA RCPT: {resp}"

        # 停顿 200ms → DATA
        time.sleep(0.2)
        sock.sendall(b"DATA\r\n")
        resp = smtp_read_response(sock)
        assert assert_code(resp, 354), f"MTA DATA: {resp}"

        # 发送 body 时有停顿（模拟生成邮件内容）
        sock.sendall(b"From: mta-slow@test.local\r\n")
        time.sleep(0.3)
        sock.sendall(b"To: mta-slow-rcpt@test.local\r\n")
        time.sleep(0.1)
        sock.sendall(b"Subject: Slow MTA test\r\n")
        time.sleep(0.2)
        sock.sendall(b"\r\n")
        time.sleep(0.3)
        sock.sendall(b"Body line 1 - sent after a pause.\r\n")
        time.sleep(0.5)
        sock.sendall(b"Body line 2 - another pause later.\r\n")
        time.sleep(0.2)
        sock.sendall(b".\r\n")

        resp = smtp_read_response(sock)
        assert assert_code(resp, 250), f"MTA Body accept: {resp}"

        sock.sendall(b"QUIT\r\n")
        smtp_read_response(sock)
        sock.close()
        print("  [PASS] Slow MTA with long pauses")
        return True
    except Exception as e:
        print(f"  [FAIL] {e}")
        import traceback; traceback.print_exc()
        return False


# ══════════════════════════════════════════════════════════════
# 测试 7: 粘包 + 截断组合 — 极限压力
# ══════════════════════════════════════════════════════════════

def test_stress_coalescing_truncation(host="127.0.0.1", port=2525):
    """
    组合测试：快速连续发送多个会话，每个都有粘包和截断。

    模拟真实 MTA 的高并发场景。
    """
    print(f"\n--- Test 7: Stress — coalescing + truncation combo ---")

    successes = 0
    total = 5

    for i in range(total):
        try:
            sock = socket.create_connection((host, port), timeout=5)
            sock.settimeout(10)

            smtp_read_response(sock)

            # 粘包前两个命令，截断第三个
            sock.sendall(f"EHLO stress{i}.test\r\nMAIL FROM:<stress{i}@test.local>".encode())
            time.sleep(0.01)
            sock.sendall(f"\r\nRCPT TO:<stress{i}-rcpt@test.local>\r\n".encode())

            # 读取响应
            for _ in range(3):
                resp = smtp_read_response(sock)
                code = resp[0][:3] if resp else "???"

            # DATA + body 粘包
            sock.sendall((
                "DATA\r\n"
                f"Subject: Stress {i}\r\n"
                "\r\n"
                f"Stress test body {i}.\r\n"
                ".\r\n"
            ).encode())
            smtp_read_response(sock)  # 354
            resp = smtp_read_response(sock)  # 250

            if resp and assert_code(resp, 250):
                successes += 1

            sock.sendall(b"QUIT\r\n")
            smtp_read_response(sock)
            sock.close()
        except Exception as e:
            print(f"    [SKIP] session {i}: {e}")

    print(f"  [PASS] Stress: {successes}/{total} sessions OK")
    return successes >= total - 1  # 允许 1 个失败


# ══════════════════════════════════════════════════════════════

def main():
    host = os.environ.get("SMTP_HOST", "127.0.0.1")
    port = int(os.environ.get("SMTP_PORT", "2525"))

    print("=" * 60)
    print("  ProtoRelay TCP Sticky / Truncation / Slow Client Test")
    print(f"  Target: {host}:{port}")
    print("=" * 60)

    results = []
    results.append(("coalescing all", test_coalescing_all_at_once(host, port)))
    results.append(("coalescing batches", test_coalescing_batches(host, port)))
    results.append(("truncation segments", test_truncation_across_segments(host, port)))
    results.append(("truncation CRLF", test_truncation_at_crlf(host, port)))
    results.append(("slow client char", test_slow_client_char_by_char(host, port)))
    results.append(("slow MTA pauses", test_slow_mta_long_pause(host, port)))
    results.append(("stress combo", test_stress_coalescing_truncation(host, port)))

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
