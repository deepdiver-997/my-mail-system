#!/usr/bin/env python3
"""
双 SMTP 服务器互通 E2E 测试。

启动两个 smtpsServer 实例：
  Server A (sender relay): 端口 10025 (MTA), 10465 (SSL), outbound 静态路由指向 B
  Server B (receiver MX):   端口 10026 (MTA, 免认证接受一切)

测试流程:
  1. 用 Python smtplib 向 Server A 投递一封发给 @b.local 的邮件
  2. Server A 通过静态路由直连 Server B 的 10026 端口完成外投
  3. 检查 Server B 的文件系统确认邮件已写入

用法:
  python test/e2e/test_dual_server.py [--server ./build/smtpsServer]
"""

import argparse, json, os, signal, socket, smtplib, subprocess, sys, tempfile, time


def load_json(path):
    with open(path) as f: return json.load(f)

def make_temp_config(base_cfg, proj_root, overrides):
    """复制一份配置到临时目录，覆盖指定字段"""
    import copy
    cfg = copy.deepcopy(base_cfg)
    cfg.update(overrides)
    cfg['perf_mode'] = False
    cfg['metrics_enabled'] = False
    cfg['log_to_console'] = False
    # 转绝对路径
    for k in ('certFile', 'keyFile', 'dhFile'):
        if cfg.get(k) and not os.path.isabs(cfg[k]):
            cfg[k] = os.path.join(proj_root, cfg[k])
    if cfg.get('db_config_file') and not os.path.isabs(cfg['db_config_file']):
        cfg['db_config_file'] = os.path.join(proj_root, cfg['db_config_file'])
    return cfg

def start_server(bin_path, cfg, mail_dir, proj_root):
    """启动服务器，返回 subprocess.Popen"""
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
    time.sleep(3)
    return proc, tmpdir

def stop_server(proc):
    try:
        os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
        proc.wait(timeout=5)
    except: pass

def verify_mail(mail_dir):
    files = [f for f in os.listdir(mail_dir)
             if f != 'att' and os.path.isfile(os.path.join(mail_dir, f))]
    assert files, f"no mail files in {mail_dir}"
    newest = max(files, key=lambda f: os.path.getmtime(os.path.join(mail_dir, f)))
    size = os.path.getsize(os.path.join(mail_dir, newest))
    assert size > 50, f"mail file too small ({size} bytes)"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--server', default='./build/smtpsServer')
    parser.add_argument('--config', default='config/smtpsConfig.json')
    args = parser.parse_args()

    script_dir = os.path.dirname(os.path.abspath(__file__))
    proj_root = os.path.normpath(os.path.join(script_dir, '..', '..'))
    server_bin = os.path.join(proj_root, args.server)
    base_cfg = load_json(os.path.join(proj_root, args.config))

    if not os.path.exists(server_bin):
        print(f"ERROR: {server_bin} not found. Build first."); sys.exit(1)

    # ---- Server A: sender relay (port 10025 MTA, 10465 SSL) ----
    # outbound 静态路由: b.local → Server B
    mail_a = tempfile.mkdtemp(prefix='smtp_a_')
    cfg_a = make_temp_config(base_cfg, proj_root, {
        'listeners': [
            {'type': 'tcp', 'port': 10025, 'auth_policy': 'off'},
            {'type': 'ssl', 'port': 10465, 'auth_policy': 'off'},
        ],
        'system_domain': 'a.local',
        'outbound': {
            'helo_domain': 'a.local',
            'ports': [25],
            'static_routes': {'b.local': {'host': '127.0.0.1', 'port': 10026}},
            'dkim': {'enabled': False},
        },
        'use_database': False,
    })

    # ---- Server B: receiver MX (port 10026 MTA, no auth) ----
    mail_b = tempfile.mkdtemp(prefix='smtp_b_')
    cfg_b = make_temp_config(base_cfg, proj_root, {
        'listeners': [
            {'type': 'tcp', 'port': 10026, 'auth_policy': 'off'},
        ],
        'system_domain': 'b.local',
        'use_database': False,
    })

    proc_a, tmp_a = None, None
    proc_b, tmp_b = None, None

    try:
        print("Starting Server A (sender) on :10025...")
        proc_a, tmp_a = start_server(server_bin, cfg_a, mail_a, proj_root)

        print("Starting Server B (receiver) on :10026...")
        proc_b, tmp_b = start_server(server_bin, cfg_b, mail_b, proj_root)

        # ---- 验证连通性 ----
        s = socket.create_connection(('127.0.0.1', 10025), timeout=3)
        banner = s.recv(512).decode()
        assert banner.startswith('220'), f"Server A banner: {banner[:60]}"
        s.close()

        s = socket.create_connection(('127.0.0.1', 10026), timeout=3)
        banner = s.recv(512).decode()
        assert banner.startswith('220'), f"Server B banner: {banner[:60]}"
        s.close()

        # ---- 通过 Server A 投递到 b.local ----
        smtp = smtplib.SMTP('127.0.0.1', 10025, timeout=10)
        smtp.ehlo('test.a.local')
        smtp.sendmail('sender@a.local', 'user@b.local',
            'From: sender@a.local\r\nTo: user@b.local\r\nSubject: Dual Server Test\r\n\r\nDelivered via static route.')
        smtp.quit()
        print("Mail submitted to Server A. Waiting for outbound delivery...")
        time.sleep(3)

        # ---- 验证 Server B 收到邮件 ----
        verify_mail(mail_b)
        print("Server B received mail ✓")

        print("\n=== Dual-server E2E test PASSED ===")

    finally:
        for p in [proc_a, proc_b]:
            if p: stop_server(p)


if __name__ == '__main__':
    main()
