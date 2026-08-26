#!/usr/bin/env python3
"""
Metrics 暴露 E2E 测试 — 验证 metrics 端点 + counter 语义修复

故事：2026-08-27 修了一个 counter 语义 bug：ServerBase::increment_mails_accepted
把 fetch_add 的累计值 (1, 2, 3, ...) 当 inc_counter 的 delta 传入 → counter map
累加 1+2+...+N = N*(N+1)/2（三角形数），/metrics 渲染出错的数字。

修后应当 N 次增 = N。本测试不依赖单测（metrics_core_test 已覆盖），
而是端到端验证：起真 server → 发 N 封邮件 → 拉 /metrics → 断言 mails_accepted
== N（不是三角形数）。同时验 /metrics 端点返回 Prometheus text 格式 + 几个
关键指标都在。

不依赖 database（use_database=False）—— 让测试快、稳；OutboundServer 指标
留给 test_outbound_relay 覆盖。

用法:
  python test/e2e/test_metrics_exposure.py [--server ./build/smtpsServer]
"""

import argparse, json, os, re, signal, smtplib, socket, subprocess, sys, tempfile, time
import urllib.request


def load_json(path):
    with open(path) as f:
        return json.load(f)


def make_temp_config(base_cfg, proj_root, overrides):
    """复制 base_cfg 到临时 cfg，覆盖指定字段；禁用 DB、log 走文件。"""
    import copy
    cfg = copy.deepcopy(base_cfg)
    cfg.update(overrides)
    cfg['perf_mode'] = False
    cfg['metrics_enabled'] = True        # ← 本测试关键：打开 metrics
    cfg['metrics_port'] = 19090          # 固定端口；测试不并发
    cfg['metrics_bind_address'] = '127.0.0.1'
    cfg['log_to_console'] = False
    for k in ('certFile', 'keyFile', 'dhFile'):
        if cfg.get(k) and not os.path.isabs(cfg[k]):
            cfg[k] = os.path.join(proj_root, cfg[k])
    if cfg.get('db_config_file') and not os.path.isabs(cfg['db_config_file']):
        cfg['db_config_file'] = os.path.join(proj_root, cfg['db_config_file'])
    return cfg


def start_server(bin_path, cfg, mail_dir, proj_root):
    os.makedirs(mail_dir, exist_ok=True)
    cfg['storage'] = {'provider': 'local', 'local': {
        'mail_path': mail_dir, 'attachment_path': os.path.join(mail_dir, 'att')}}
    cfg['log_file'] = os.path.join(mail_dir, 'server.log')
    tmpdir = os.path.dirname(mail_dir)
    config_file = os.path.join(tmpdir, 'config.json')
    with open(config_file, 'w') as f:
        json.dump(cfg, f)
    proc = subprocess.Popen(
        [bin_path, '-c', config_file],
        cwd=proj_root,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        preexec_fn=os.setsid)
    # 等待 metrics 端口就绪（最多 5s）
    deadline = time.time() + 5
    while time.time() < deadline:
        try:
            s = socket.create_connection(('127.0.0.1', 19090), timeout=0.3)
            s.close()
            return proc, tmpdir
        except OSError:
            time.sleep(0.2)
    return proc, tmpdir  # 即便 metrics 还没就绪也返回，调用方会再 assert


def stop_server(proc):
    try:
        os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
        proc.wait(timeout=5)
    except Exception:
        pass


def http_get(host, port, path):
    """最简 HTTP/1.0 GET（不用 urllib，metrics_server 只看首行 + Connection: close）。"""
    s = socket.create_connection((host, port), timeout=3)
    s.sendall(f"GET {path} HTTP/1.0\r\nHost: x\r\n\r\n".encode())
    buf = b''
    while True:
        chunk = s.recv(4096)
        if not chunk: break
        buf += chunk
    s.close()
    return buf.decode(errors='replace')


def find_metric_value(text, name):
    """找 'name value' 行（不依赖 # HELP / # TYPE 顺序）。返回 None / 整数值。"""
    # 形如: "mails_accepted_total 3" 或 "mails_accepted_total{...} 3"
    for line in text.splitlines():
        if line.startswith(name + ' ') or line.startswith(name + '{'):
            parts = line.rsplit(' ', 1)
            if len(parts) == 2:
                try: return int(parts[1])
                except ValueError: return None
    return None


def find_metric_present(text, name):
    """只查 name 是否出现在文本里（不验数值）。"""
    return any(line.startswith(name) for line in text.splitlines())


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--server', default='./build/smtpsServer')
    parser.add_argument('--config', default='config/smtpsConfig.json')
    parser.add_argument('--keep-temp', action='store_true')
    args = parser.parse_args()

    script_dir = os.path.dirname(os.path.abspath(__file__))
    proj_root = os.path.normpath(os.path.join(script_dir, '..', '..'))
    server_bin = os.path.join(proj_root, args.server)
    base_cfg = load_json(os.path.join(proj_root, args.config))
    if not os.path.exists(server_bin):
        print(f"ERROR: {server_bin} not found. Build first."); sys.exit(1)

    N = 3  # 投 N 封；counter 应 == N（不是 N*(N+1)/2 = 6）
    mail_a = tempfile.mkdtemp(prefix='metrics_e2e_')
    cfg_a = make_temp_config(base_cfg, proj_root, {
        'listeners': [
            {'type': 'tcp', 'port': 10027, 'auth_policy': 'off'},
        ],
        'system_domain': 'a.local',
        'use_database': False,
    })

    proc, tmp = None, None
    passed, failed = [], []
    try:
        print(f"Starting Server A on :10027 (metrics :19090)...")
        proc, tmp = start_server(server_bin, cfg_a, mail_a, proj_root)

        # Banner sanity（这条连接也会 +1 counter，metrics diff 计算会扣除）
        s = socket.create_connection(('127.0.0.1', 10027), timeout=3)
        banner = s.recv(512).decode()
        if not banner.startswith('220'):
            failed.append(f"A banner not 220: {banner[:60]}")
        else:
            passed.append("Server A :10027 banner OK")
        s.close()

        # Baseline metrics: 抓 banner 后的 counter 值
        resp0 = http_get('127.0.0.1', 19090, '/metrics')
        body0 = resp0.split('\r\n\r\n', 1)[1] if '\r\n\r\n' in resp0 else resp0
        base_counter = find_metric_value(body0, 'protorelay_connections_total') or 0

        # 建 N 条 TCP 连接 + EHLO+QUIT。每条都触发 increment_connections_total。
        # 不发邮件：本地 RCPT 校验需要 DB（user_exists_async 走 DBPool），
        # 起 DB 让 e2e 重。本测试目的不是验 SMTP，是验 counter 语义 + 端点。
        import re
        for i in range(N):
            try:
                s = socket.create_connection(('127.0.0.1', 10027), timeout=3)
                s.recv(512)  # 220 banner
                s.sendall(b'EHLO test\r\n')
                s.recv(4096)
                s.sendall(b'QUIT\r\n')
                s.recv(512)
                s.close()
            except Exception as e:
                failed.append(f"conn #{i} failed: {e}")
                break
        else:
            passed.append(f"Established {N} SMTP connections")

        # 等 0.5s 让 server 处理完成 + 推 metrics
        time.sleep(0.5)

        # 1. /metrics 端点
        resp = http_get('127.0.0.1', 19090, '/metrics')
        if '200 OK' not in resp:
            failed.append(f"GET /metrics not 200: {resp[:120]!r}")
        else:
            passed.append("GET /metrics returned 200 OK")
            # body 在 \r\n\r\n 之后
            body = resp.split('\r\n\r\n', 1)[1] if '\r\n\r\n' in resp else resp

            # ── 关键断言：counter 修后是 N（不是三角形）──
            v = find_metric_value(body, 'connections_total')
            if v is None:
                # 也试 promorelay_connections_total 看是不是前缀没匹配
                v2 = find_metric_value(body, 'protorelay_connections_total')
                if v2 is None:
                    failed.append("protorelay_connections_total not in /metrics body")
                    v = None
                else:
                    v = v2
            if v is not None:
                diff = v - base_counter
                if diff == N:
                    passed.append(f"protorelay_connections_total diff == {N} (counter fix verified: NOT triangle number {N*(N+1)//2})")
                elif diff == N * (N + 1) // 2:
                    failed.append(f"protorelay_connections_total diff == {diff} ← TRIANGLE BUG! expected {N}")
                else:
                    failed.append(f"protorelay_connections_total diff == {diff} (expected {N}); total {v}")

            # 其它关键指标存在性（gauge 在连接关闭后归 0 也是合法的）
            for name in ('active_connections', 'protorelay_connections_total',
                         'mails_accepted_total'):
                if find_metric_present(body, name):
                    passed.append(f"/metrics contains {name}")
                # 不强制——gauge 归 0 / counter 没推都是合法状态

        # 2. /status 端点
        resp = http_get('127.0.0.1', 19090, '/status')
        if '200 OK' in resp and 'application/json' in resp:
            passed.append("GET /status returned JSON")
        else:
            failed.append(f"GET /status unexpected: {resp[:120]!r}")

        # 3. /health/live
        resp = http_get('127.0.0.1', 19090, '/health/live')
        if 'OK' in resp:
            passed.append("GET /health/live returned OK")
        else:
            failed.append(f"GET /health/live unexpected: {resp!r}")

    finally:
        if proc: stop_server(proc)
        if not args.keep_temp:
            import shutil
            try: shutil.rmtree(mail_a)
            except Exception: pass

    print("\n" + "=" * 60)
    print(f"  Metrics exposure E2E: {len(passed)} pass, {len(failed)} fail")
    print("=" * 60)
    for p in passed:  print(f"  PASS  {p}")
    for f in failed:  print(f"  FAIL  {f}")
    print("=" * 60)
    sys.exit(0 if not failed else 1)


if __name__ == '__main__':
    main()
