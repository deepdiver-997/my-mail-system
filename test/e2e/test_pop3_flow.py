#!/usr/bin/env python3
"""
POP3 E2E 流程测试 — TCP-only（不依赖 DB）

启动真实 pop3Server（use_database=False），验证：
  1. 协议流程（RFC 1939）：
       greeting 格式 `+OK ProtoRelay POP3 server ready <ts@pop3>`
       CAPA 多行能力列表 / USER / PASS（无 DB → 预期 -ERR）/ QUIT
  2. Metrics 端点：
       protorelay_pop3_sessions_total{result=ok/err} 随会话关闭递增
       protorelay_pop3_auth_total{result=wrong_pass} 随 PASS 失败递增
     retr/dele/lock_conflict 三个指标需要 TRANSACTION + DB（PASS 成功后
     才有 RETR/DELE/抢锁路径），本 e2e 不依赖 DB，由 pop3_fsm_test 单测
     覆盖对应代码路径；push 机制本身与 sessions/auth 共用同一 inc_counter。

用法:
  python test/e2e/test_pop3_flow.py [--server ./build/pop3Server] [--keep-temp]
"""

import argparse, json, os, re, signal, socket, subprocess, sys, tempfile, time


def load_json(path):
    with open(path) as f:
        return json.load(f)


def make_temp_config(base_cfg, proj_root, overrides):
    """复制 base_cfg 到临时 cfg，覆盖监听/日志/metrics；禁用 DB。"""
    import copy
    cfg = copy.deepcopy(base_cfg)
    cfg.update(overrides)
    cfg['perf_mode'] = False
    cfg['use_database'] = False
    cfg['metrics_enabled'] = True
    cfg['metrics_port'] = 19090
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
    # 等待 POP3 端口就绪（最多 5s）
    listener_port = cfg['listeners'][0]['port']
    deadline = time.time() + 5
    while time.time() < deadline:
        try:
            s = socket.create_connection(('127.0.0.1', listener_port), timeout=0.3)
            s.close()
            return proc, tmpdir
        except OSError:
            time.sleep(0.2)
    return proc, tmpdir


def stop_server(proc):
    try:
        os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
        proc.wait(timeout=5)
    except Exception:
        pass


def http_get(host, port, path):
    """最简 HTTP/1.0 GET（metrics_server 只看首行 + Connection: close）。"""
    s = socket.create_connection((host, port), timeout=3)
    s.sendall(f"GET {path} HTTP/1.0\r\nHost: x\r\n\r\n".encode())
    buf = b''
    while True:
        chunk = s.recv(4096)
        if not chunk:
            break
        buf += chunk
    s.close()
    return buf.decode(errors='replace')


def find_metric_value(text, name):
    """找 'name{labels} value' 行（name 可带 label 前缀过滤）。返回 None / 整数值。"""
    for line in text.splitlines():
        if line.startswith(name + ' ') or line.startswith(name + '{'):
            parts = line.rsplit(' ', 1)
            if len(parts) == 2:
                try:
                    return int(parts[1])
                except ValueError:
                    return None
    return None


def recv_line(s):
    """读一行（CRLF 或 LF 结尾），返回去行尾的 str。"""
    buf = b''
    while b'\n' not in buf:
        chunk = s.recv(512)
        if not chunk:
            break
        buf += chunk
    return buf.split(b'\n')[0].rstrip(b'\r').decode(errors='replace')


def recv_multiline(s):
    """读多行响应直到单独一个 '.' 终止行（CAPA / LIST / UIDL）。"""
    buf = b''
    while True:
        chunk = s.recv(512)
        if not chunk:
            break
        buf += chunk
        if any(line.rstrip(b'\r\n') == b'.' for line in buf.split(b'\n')):
            break
    return buf.decode(errors='replace')


def cmd(s, payload, multiline=False):
    s.sendall(payload)
    return recv_multiline(s) if multiline else recv_line(s)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--server', default='./build/pop3Server')
    parser.add_argument('--config', default='config/pop3Config.json')
    parser.add_argument('--listener-port', type=int, default=10110)
    parser.add_argument('--keep-temp', action='store_true')
    args = parser.parse_args()

    script_dir = os.path.dirname(os.path.abspath(__file__))
    proj_root = os.path.normpath(os.path.join(script_dir, '..', '..'))
    server_bin = os.path.join(proj_root, args.server)
    base_cfg = load_json(os.path.join(proj_root, args.config))
    if not os.path.exists(server_bin):
        print(f"ERROR: {server_bin} not found. Build first."); sys.exit(1)

    mail_dir = tempfile.mkdtemp(prefix='pop3_e2e_')
    cfg = make_temp_config(base_cfg, proj_root, {
        'listeners': [{'type': 'tcp', 'port': args.listener_port, 'auth_policy': 'off'}],
        'system_domain': 'test.local',
    })

    proc = None
    passed, failed, skipped = [], [], []
    try:
        print(f"Starting pop3Server on :{args.listener_port} (metrics :19090, no DB)...")
        proc, tmp = start_server(server_bin, cfg, mail_dir, proj_root)

        # ============ 1. 协议流程 ============
        s = socket.create_connection(('127.0.0.1', args.listener_port), timeout=5)

        # greeting 格式
        greeting = recv_line(s)
        m = re.match(r'^\+OK ProtoRelay POP3 server ready <\d+@pop3>$', greeting)
        if m:
            passed.append(f"greeting OK: {greeting}")
        else:
            failed.append(f"greeting format: {greeting!r}")

        # CAPA（多行，以 '.' 结束）
        resp = cmd(s, b'CAPA\r\n', multiline=True)
        if ('+OK Capability list follows' in resp and '\nUSER\r\n' in resp
                and '\nUIDL\r\n' in resp and resp.rstrip('\r\n').endswith('\n.')):
            passed.append("CAPA lists USER/UIDL + '.' terminator")
        else:
            failed.append(f"CAPA unexpected: {resp[:120]!r}")

        # USER
        resp = cmd(s, b'USER alice@test.local\r\n')
        if resp == '+OK Send PASS':
            passed.append("USER -> +OK Send PASS")
        else:
            failed.append(f"USER unexpected: {resp!r}")

        # PASS（无 DB → auth_user 无池 → -ERR）
        resp = cmd(s, b'PASS test123\r\n')
        if resp.startswith('-ERR'):
            passed.append("PASS (no DB) -> -ERR")
        else:
            failed.append(f"PASS should fail without DB: {resp!r}")

        # QUIT
        resp = cmd(s, b'QUIT\r\n')
        if resp == '+OK Bye':
            passed.append("QUIT -> +OK Bye")
        else:
            failed.append(f"QUIT unexpected: {resp!r}")
        s.close()

        # 一条异常断连会话 → sessions_total{result=err}
        s2 = socket.create_connection(('127.0.0.1', args.listener_port), timeout=5)
        recv_line(s2)   # 收 greeting 后直接断开（不发 QUIT）
        s2.close()
        time.sleep(0.5)   # 等服务器检测 EOF 并关闭会话

        # ============ 2. Metrics 端点 ============
        resp = http_get('127.0.0.1', 19090, '/metrics')
        body = resp.split('\r\n\r\n', 1)[1] if '\r\n\r\n' in resp else resp

        ok = find_metric_value(body, 'protorelay_pop3_sessions_total{result="ok"}')
        err = find_metric_value(body, 'protorelay_pop3_sessions_total{result="err"}')
        if ok is not None and ok >= 1:
            passed.append(f"sessions_total{{result=ok}} = {ok}")
        else:
            failed.append(f"sessions_total{{result=ok}} missing (body={body[:200]!r})")
        if err is not None and err >= 1:
            passed.append(f"sessions_total{{result=err}} = {err}")
        else:
            failed.append(f"sessions_total{{result=err}} missing/0 (body={body[:200]!r})")

        auth = find_metric_value(body, 'protorelay_pop3_auth_total')
        if auth is not None and auth >= 1:
            passed.append(f"auth_total = {auth} (PASS 失败递增)")
        else:
            failed.append(f"auth_total missing/0 (body={body[:200]!r})")

        # retr/dele/lock_conflict 需要 TRANSACTION + DB → e2e 无法触发
        for name in ('protorelay_pop3_retr_total', 'protorelay_pop3_dele_total',
                     'protorelay_pop3_lock_conflict_total'):
            skipped.append(f"{name} (needs DB, covered by pop3_fsm_test)")

        # /status 端点基本可用
        resp = http_get('127.0.0.1', 19090, '/status')
        if '200 OK' in resp and 'application/json' in resp:
            passed.append("GET /status returned JSON")
        else:
            failed.append(f"GET /status unexpected: {resp[:120]!r}")

    finally:
        if proc:
            stop_server(proc)
        if not args.keep_temp:
            import shutil
            try:
                shutil.rmtree(mail_dir)
            except Exception:
                pass

    print("\n" + "=" * 60)
    print(f"  POP3 E2E: {len(passed)} pass, {len(failed)} fail, {len(skipped)} skip")
    print("=" * 60)
    for p in passed:
        print(f"  PASS  {p}")
    for f in failed:
        print(f"  FAIL  {f}")
    for s in skipped:
        print(f"  SKIP  {s}")
    sys.exit(0 if not failed else 1)


if __name__ == '__main__':
    main()
