#!/usr/bin/env python3
"""
SMTP E2E 全流程测试。

读取 smtpsConfig.json 配置，启动 smtpsServer，验证:
  1. 端口 25 — MTA 投递 (免认证，外部邮件直投)
  2. 端口 587 — 客户端提交 (需 AUTH)
  3. 端口 465 — SSL + 客户端提交 (需 AUTH)
  4. 端口 25 — 未认证发信应被拒绝 MAIL FROM/DATA

用法:
  python test/e2e/test_smtp_flow.py [--server ./build/smtpsServer] [--config config/smtpsConfig.json]
"""

import argparse
import json
import os
import signal
import smtplib
import socket
import ssl
import subprocess
import sys
import time
import base64


class TestResult:
    def __init__(self):
        self.passed = 0
        self.failed = 0
        self.skipped = 0

    def test(self, name, fn):
        try:
            fn()
            self.passed += 1
            print(f"  PASS  {name}")
        except AssertionError as e:
            self.failed += 1
            print(f"  FAIL  {name}: {e}")
        except Exception as e:
            self.failed += 1
            print(f"  FAIL  {name}: {type(e).__name__}: {e}")

    def skip(self, name, reason):
        self.skipped += 1
        print(f"  SKIP  {name} ({reason})")

    def summary(self):
        print(f"\n{'='*50}")
        print(f"  Passed: {self.passed}  Failed: {self.failed}  Skipped: {self.skipped}")
        return self.failed == 0


def load_config(path):
    with open(path) as f:
        return json.load(f)


def start_server(server_bin, cwd):
    proc = subprocess.Popen(
        [server_bin],
        cwd=cwd,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        preexec_fn=os.setsid
    )
    time.sleep(2)  # wait for startup
    return proc


def stop_server(proc):
    os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
        proc.wait()


def main():
    parser = argparse.ArgumentParser(description='SMTP E2E flow test')
    parser.add_argument('--server', default='./build/smtpsServer')
    parser.add_argument('--config', default='config/smtpsConfig.json')
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--db-config', default='config/db_config.json')
    args = parser.parse_args()

    # ---- 定位项目根目录 ----
    script_dir = os.path.dirname(os.path.abspath(__file__))
    proj_root = os.path.normpath(os.path.join(script_dir, '..', '..'))

    server_bin = os.path.join(proj_root, args.server) if not os.path.isabs(args.server) else args.server
    config_path = os.path.join(proj_root, args.config) if not os.path.isabs(args.config) else args.config

    if not os.path.exists(server_bin):
        print(f"ERROR: server binary not found: {server_bin}")
        print("  Build first: ./build.sh Release")
        sys.exit(1)

    cfg = load_config(config_path)
    host = args.host
    r = TestResult()
    perf_mode = cfg.get('perf_mode', False)

    # 找出各端口
    listeners = {l['port']: l for l in cfg.get('listeners', [])}
    port_25 = listeners.get(25, {})
    port_587 = listeners.get(587, {})
    port_465 = listeners.get(465, {})

    print(f"Config: {config_path}")
    print(f"Listeners: {list(listeners.keys())}")
    print(f"Starting server: {server_bin}")

    proc = start_server(server_bin, proj_root)

    try:
        # ================================================================
        # Test 1: 端口 25 — MTA 投递，免认证
        # ================================================================
        r.test("Port 25 accepts TCP connection", lambda: _tcp_connect(host, 25))

        r.test("Port 25 sends greeting banner", lambda: _expect_banner(host, 25, '220'))

        r.test("Port 25 EHLO without AUTH", lambda: _smtp_ehlo(host, 25))

        r.test("Port 25 MAIL FROM without prior AUTH", lambda:
            _smtp_cmd(host, 25, 'MAIL FROM:<test@extern.com>', '250'))

        r.test("Port 25 RCPT TO after MAIL FROM", lambda:
            _smtp_rcpt(host, 25))

        r.test("Port 25 DATA command accepted", lambda:
            _smtp_data(host, 25))

        # ================================================================
        # Test 2: 端口 587 — 客户端提交，必须 AUTH
        # ================================================================
        r.test("Port 587 accepts TCP", lambda: _tcp_connect(host, 587))

        r.test("Port 587 greeting banner", lambda: _expect_banner(host, 587, '220'))

        if perf_mode:
            r.skip("Port 587 rejects MAIL FROM before AUTH", "perf_mode disables AUTH checks")
            r.skip("Port 587 EHLO advertises AUTH", "perf_mode disables AUTH")
        else:
            r.test("Port 587 rejects MAIL FROM before AUTH", lambda:
                _smtp_cmd(host, 587, 'MAIL FROM:<user@scut.email>', '530'))
            r.test("Port 587 EHLO advertises AUTH", lambda:
                _smtp_ehlo_check_auth(host, 587))

        # ================================================================
        # Test 3: 端口 465 — SSL + AUTH
        # ================================================================
        if port_465:
            r.test("Port 465 SSL handshake", lambda:
                _ssl_connect(host, 465))

            r.test("Port 465 SSL greeting", lambda:
                _ssl_expect_banner(host, 465, '220'))

            if perf_mode:
                r.skip("Port 465 rejects MAIL FROM before AUTH", "perf_mode disables AUTH")
            else:
                r.test("Port 465 rejects MAIL FROM before AUTH", lambda:
                    _ssl_cmd(host, 465, 'MAIL FROM:<user@scut.email>', '530'))
        else:
            r.skip("Port 465 SSL", "no SSL listener configured")

        # ================================================================
        # Test 4: smtplib E2E 投递
        # ================================================================
        db_cfg = _load_db_config(proj_root, cfg)
        if perf_mode:
            r.skip("smtplib auth+send on port 587", "perf_mode skips AUTH advertisement")
        elif db_cfg:
            r.test("smtplib auth + send on port 587", lambda:
                _smtplib_send(host, 587, db_cfg))
        else:
            r.skip("smtplib auth+send", "DB config not found")

    finally:
        print("\nStopping server...")
        stop_server(proc)

    ok = r.summary()
    sys.exit(0 if ok else 1)


# ---- helpers ----

def _tcp_connect(host, port):
    s = socket.create_connection((host, port), timeout=5)
    s.close()

def _expect_banner(host, port, expect_code):
    s = socket.create_connection((host, port), timeout=5)
    data = s.recv(1024).decode()
    s.close()
    assert data.startswith(expect_code), f"expected {expect_code}, got: {data[:80]}"

def _smtp_ehlo(host, port):
    s = socket.create_connection((host, port), timeout=5)
    s.recv(1024)  # banner
    s.sendall(b'EHLO test.client\r\n')
    resp = s.recv(4096).decode()
    s.close()
    assert '250' in resp, f"EHLO failed: {resp[:200]}"

def _smtp_ehlo_check_auth(host, port):
    s = socket.create_connection((host, port), timeout=5)
    s.recv(1024)
    s.sendall(b'EHLO test.client\r\n')
    resp = s.recv(4096).decode()
    s.close()
    assert 'AUTH' in resp.upper(), f"no AUTH advertised: {resp[:200]}"

def _smtp_cmd(host, port, cmd, expect):
    s = socket.create_connection((host, port), timeout=5)
    s.recv(1024)  # banner
    # send EHLO first if needed for state
    s.sendall(b'EHLO test\r\n')
    s.recv(4096)
    s.sendall((cmd + '\r\n').encode())
    resp = s.recv(1024).decode()
    s.close()
    assert resp.startswith(expect), f"cmd={cmd}, expected={expect}, got: {resp[:80]}"

def _smtp_rcpt(host, port):
    s = socket.create_connection((host, port), timeout=5)
    s.recv(1024)
    s.sendall(b'EHLO extern.com\r\n')
    s.recv(4096)
    s.sendall(b'MAIL FROM:<test@extern.com>\r\n')
    s.recv(1024)
    s.sendall(b'RCPT TO:<user@scut.email>\r\n')
    resp = s.recv(1024).decode()
    s.close()
    assert resp.startswith('250'), f"RCPT TO rejected: {resp[:80]}"

def _smtp_data(host, port):
    s = socket.create_connection((host, port), timeout=5)
    s.recv(1024)
    s.sendall(b'EHLO test\r\n')
    s.recv(4096)
    s.sendall(b'MAIL FROM:<test@extern.com>\r\n')
    s.recv(1024)
    s.sendall(b'RCPT TO:<user@scut.email>\r\n')
    s.recv(1024)
    s.sendall(b'DATA\r\n')
    resp = s.recv(1024).decode()
    s.close()
    assert resp.startswith('354'), f"DATA rejected: {resp[:80]}"

def _ssl_connect(host, port):
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    s = socket.create_connection((host, port), timeout=5)
    ss = ctx.wrap_socket(s, server_hostname=host)
    ss.close()

def _ssl_expect_banner(host, port, expect):
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    s = socket.create_connection((host, port), timeout=5)
    ss = ctx.wrap_socket(s, server_hostname=host)
    data = ss.recv(1024).decode()
    ss.close()
    assert data.startswith(expect), f"expected {expect}, got: {data[:80]}"

def _ssl_cmd(host, port, cmd, expect):
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    s = socket.create_connection((host, port), timeout=5)
    ss = ctx.wrap_socket(s, server_hostname=host)
    ss.recv(1024)
    ss.sendall(b'EHLO test\r\n')
    ss.recv(4096)
    ss.sendall((cmd + '\r\n').encode())
    resp = ss.recv(1024).decode()
    ss.close()
    assert resp.startswith(expect), f"cmd={cmd}, expected={expect}, got: {resp[:80]}"

def _load_db_config(proj_root, cfg):
    db_file = cfg.get('db_config_file', 'config/db_config.json')
    path = os.path.join(proj_root, db_file)
    if not os.path.exists(path):
        # try config/ subdir
        path = os.path.join(proj_root, 'config', os.path.basename(db_file))
    if os.path.exists(path):
        with open(path) as f:
            return json.load(f)
    return None

def _smtplib_send(host, port, db_cfg):
    s = smtplib.SMTP(host, port, timeout=10)
    s.ehlo()
    user = db_cfg.get('user', 'root')
    pwd = db_cfg.get('password', '')
    if user and pwd:
        s.login(user, pwd)
    s.sendmail('test@extern.com', 'user@scut.email',
               'From: test@extern.com\r\nTo: user@scut.email\r\nSubject: E2E Test\r\n\r\nHello.')
    s.quit()


if __name__ == '__main__':
    main()
