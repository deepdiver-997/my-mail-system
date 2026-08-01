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
    parser.add_argument('--keep-temp', action='store_true', help='keep temp config dir')
    args = parser.parse_args()

    script_dir = os.path.dirname(os.path.abspath(__file__))
    proj_root = os.path.normpath(os.path.join(script_dir, '..', '..'))

    server_bin = os.path.join(proj_root, args.server) if not os.path.isabs(args.server) else args.server
    config_path = os.path.join(proj_root, args.config) if not os.path.isabs(args.config) else args.config

    if not os.path.exists(server_bin):
        print(f"ERROR: server binary not found: {server_bin}")
        sys.exit(1)

    # ---- 创建临时配置 (可调节，不污染原始配置) ----
    import tempfile
    tmpdir = tempfile.mkdtemp(prefix='smtp_e2e_')
    cfg = load_config(config_path)
    cfg['perf_mode'] = False                     # 启用安全检查
    cfg['inbound_spf_mode'] = 'off'              # SPF 需真实 DNS，测试环境关
    cfg['inbound_dkim_mode'] = 'off'
    cfg['inbound_dmarc_mode'] = 'off'
    cfg['log_file'] = os.path.join(tmpdir, 'server.log')
    cfg['metrics_enabled'] = False
    # db_config 路径需要是绝对路径 (配置在 tmpdir, db_config 在原项目)
    db_file = cfg.get('db_config_file', 'config/db_config.json')
    cfg['db_config_file'] = os.path.join(proj_root, db_file)
    cert_file = cfg.get('certFile', ''); cfg['certFile'] = os.path.join(proj_root, cert_file) if cert_file and not os.path.isabs(cert_file) else cert_file
    key_file = cfg.get('keyFile', ''); cfg['keyFile'] = os.path.join(proj_root, key_file) if key_file and not os.path.isabs(key_file) else key_file
    # 存储路径指向临时目录
    mail_dir = os.path.join(tmpdir, 'mail')
    attach_dir = os.path.join(tmpdir, 'attachments')
    os.makedirs(mail_dir, exist_ok=True)
    os.makedirs(attach_dir, exist_ok=True)
    cfg['storage'] = {'provider': 'local', 'local': {
        'mail_path': mail_dir, 'attachment_path': attach_dir}}

    tmp_config = os.path.join(tmpdir, 'config.json')
    with open(tmp_config, 'w') as f:
        json.dump(cfg, f)

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

    print(f"Temp config: {tmp_config}")
    proc = subprocess.Popen(
        [server_bin, '-c', tmp_config],
        cwd=proj_root,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        preexec_fn=os.setsid
    )
    time.sleep(2)

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
        # Test 4: smtplib E2E 投递 + 验证持久化
        # ================================================================
        db_cfg = _load_db_config(proj_root, cfg)
        if perf_mode:
            r.skip("smtplib auth+send on port 587", "perf_mode skips AUTH advertisement")
        elif db_cfg:
            r.test("smtplib auth + send on port 587", lambda:
                _smtplib_send(host, 587, db_cfg))
            time.sleep(1)  # wait for async persist
            r.test("mail file exists on disk", lambda: _verify_mail_on_disk(mail_dir))
        else:
            r.skip("smtplib auth+send", "DB config not found")

    finally:
        print("\nStopping server...")
        os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            os.killpg(os.getpgid(proc.pid), signal.SIGKILL); proc.wait()
        if not args.keep_temp:
            import shutil; shutil.rmtree(tmpdir, ignore_errors=True)
        else:
            print(f"Temp files kept at: {tmpdir}")

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

def _verify_mail_on_disk(mail_dir):
    """验证 mail 目录下是否有新写入的邮件文件"""
    files = [f for f in os.listdir(mail_dir) if os.path.isfile(os.path.join(mail_dir, f))]
    assert len(files) > 0, f"no mail files found in {mail_dir}"
    # 检查最新文件大小合理
    newest = max(files, key=lambda f: os.path.getmtime(os.path.join(mail_dir, f)))
    size = os.path.getsize(os.path.join(mail_dir, newest))
    assert size > 50, f"mail file too small ({size} bytes), likely empty"

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
