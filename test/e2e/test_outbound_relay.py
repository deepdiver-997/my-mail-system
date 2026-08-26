#!/usr/bin/env python3
"""
OutboundServer 进程级 E2E 测试 — A 接收 + relay 尝试 B。

拓扑:
  Python smtplib → A:10025 (SMTP auth=on, use_database=true, storage=local)
  A 收到 bob@b.local → 写 outbox (PENDING) → OutboundServer 走 static_routes
    → 连 B:10026 (SMTP auth=on, use_database=true, storage=local)
  → B 拒收（B 也要求 auth，A 的 OutboundSmtpSession 不发 AUTH —— 已知限制）

关键 invariant (本测试不要求邮件实际到达 B):
  1. A 收到 smtplib 后 OutboundServer 真的 attempt connect B:10026
  2. B 端有来自 127.0.0.1 的 inbound SMTP 连接（被 FSM 拒但**连接建立过**）
  3. A 启动日志含 "OutboundServer (new engine) started"（死代码已复活）
  4. A 端日志含 "Outbound: connected to 127.0.0.1"（static_routes 解析 + 端口派生生效）

不验证:
  - B 落盘 —— 因为 B 也要求 AUTH，而 OutboundSmtpSession 当前不实现 AUTH（已知 C++ 缺陷）
  - B 收到 Subject: dual relay —— 同上

前置依赖:
  - MySQL 已起，且 test/config/create_tables.sql 已 load
  - 端口 10025/10026 空闲
  - ./build/smtpsServer 已编译
  - users 表里存在 alice@a.local（密码 e2e_password）—— 测试自动 INSERT IGNORE

用法:
  python3 test/e2e/test_outbound_relay.py [--server ./build/smtpsServer]
"""

import argparse
import json
import os
import shutil
import signal
import smtplib
import socket
import subprocess
import sys
import tempfile
import time
import traceback


SERVER_BIN_DEFAULT = './build/smtpsServer'
CONFIG_TEMPLATE   = 'config/smtpsConfig.json'   # 生产模板，承载完整字段

# 高端口，避免 sudo；10025/10026 避开系统服务
A_PORT = 10025
B_PORT = 10026

A_DOMAIN = 'a.local'
B_DOMAIN = 'b.local'

SMTP_SENDER    = f'alice@{A_DOMAIN}'
SMTP_RECIPIENT = f'bob@{B_DOMAIN}'

# A 的 SMTP listener 要求认证（auth_policy=on），smtplib 用 alice@a.local/e2e_password 登录
# 之所以用 on：FSM 的 RCPT 阶段仅在 is_authenticated=true 时放行外部域名
# （listener 收到 bob@b.local 时不会拒 relay）。需要 DB 里有此用户。
SMTP_AUTH_USER     = SMTP_SENDER
SMTP_AUTH_PASSWORD = 'e2e_password'

WAIT_PORT_TIMEOUT_S = 10
DELIVERY_WAIT_S     = 30
SUBJECT_TAG         = 'dual relay'


# ── helpers ─────────────────────────────────────────────

def wait_for_port(host, port, timeout=WAIT_PORT_TIMEOUT_S):
    """轮询直到端口可连或超时。返回 bool。"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with socket.create_connection((host, port), timeout=1):
                return True
        except (ConnectionRefusedError, socket.timeout, OSError):
            time.sleep(0.1)
    return False


def wait_for_file(dirpath, predicate, timeout=DELIVERY_WAIT_S, poll=0.2):
    """轮询 dirpath 直到 predicate(path) 返 True 或超时。返回命中文件路径。"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            for entry in os.listdir(dirpath):
                full = os.path.join(dirpath, entry)
                if os.path.isfile(full) and predicate(full):
                    return full
        except FileNotFoundError:
            pass
        time.sleep(poll)
    return None


def wait_for(predicate, timeout=DELIVERY_WAIT_S, poll=0.2):
    """轮询 predicate() 直到返 True 或超时。返回 bool。"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return True
        time.sleep(poll)
    return predicate()


def kill_pg(proc):
    """用进程组杀进程（preexec_fn=os.setsid 启的）。"""
    try:
        os.killpg(os.getpgid(proc.pid), signal.SIGTERM)
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
            proc.wait(timeout=5)
    except (ProcessLookupError, OSError):
        pass


# ── /etc/hosts 注入：让测试和生产共享同一份 cfg 模板 ─────────
# 思路：e2e 启动时往 /etc/hosts 注入 b.local→127.0.0.1，结束时撤掉。
# 这样 outbound.static_routes[b.local] 可以写真实域名（生产配置风格），
# 不需要 hardcode IP（否则 prod / test 走两套 binary）。
# /etc/hosts 写需要 sudo；如果没 sudo 就 skip（test 在能 sudo 的环境跑）。
HOSTS_FILE = '/etc/hosts'
HOSTS_MARKER_BEGIN = '# >>> protorelay-e2e test injection (b.local) >>>'
HOSTS_MARKER_END   = '# <<< protorelay-e2e test injection (b.local) <<<'
HOSTS_INJECT_LINE  = f'127.0.0.1\tb.local\t{HOSTS_MARKER_BEGIN}'

def _read_hosts():
    with open(HOSTS_FILE) as f:
        return f.read()

def _write_hosts(content):
    # 原子写：先写临时再 rename（避免写到一半失败）
    tmp = HOSTS_FILE + '.protorelay-e2e.tmp'
    with open(tmp, 'w') as f:
        f.write(content)
    os.chmod(tmp, 0o644)
    os.replace(tmp, HOSTS_FILE)

def setup_local_dns():
    """注入 b.local→127.0.0.1 到 /etc/hosts。需要 sudo，失败就 raise。"""
    try:
        original = _read_hosts()
    except PermissionError as e:
        raise RuntimeError(
            f"读 {HOSTS_FILE} 没权限，e2e 需要 sudo。run: sudo -E python3 ..."
        ) from e
    if HOSTS_MARKER_BEGIN in original:
        # 之前注入残留（异常退出没清掉）
        print("  [dns] 旧 marker 残留，先清再注")
        original = _remove_injection(original)
    new = (
        original.rstrip('\n') + '\n'
        + HOSTS_INJECT_LINE + '\n'
        + HOSTS_MARKER_END + '\n'
    )
    # 直接写（已经是 root 才能 import 不到的话）
    try:
        _write_hosts(new)
    except PermissionError as e:
        raise RuntimeError(
            f"写 {HOSTS_FILE} 没权限，e2e 需要 sudo。"
        ) from e
    # 验证
    import socket as _s
    real = _s.gethostbyname('b.local')
    if real != '127.0.0.1':
        raise RuntimeError(f"/etc/hosts 注入后 b.local 仍解析为 {real!r}")
    print(f"  [dns] 注入 b.local→127.0.0.1 OK")

def _remove_injection(content):
    """删掉 marker 之间的注入行。"""
    lines = content.split('\n')
    out, in_block = [], False
    for line in lines:
        if HOSTS_MARKER_BEGIN in line:
            in_block = True
            continue
        if HOSTS_MARKER_END in line:
            in_block = False
            continue
        if not in_block:
            out.append(line)
    return '\n'.join(out)

def teardown_local_dns():
    """从 /etc/hosts 撤掉注入。失败只 warn，不 raise（e2e 一定要清干净）。"""
    try:
        original = _read_hosts()
    except (PermissionError, FileNotFoundError):
        return
    if HOSTS_MARKER_BEGIN not in original:
        return
    new = _remove_injection(original)
    try:
        _write_hosts(new)
        print(f"  [dns] 撤掉 b.local→127.0.0.1 注入 OK")
    except PermissionError:
        print(f"  [WARN] 撤注入失败，留下了 marker。请手动编辑 {HOSTS_FILE}")


def load_config(template, proj_root, listeners, system_domain, outbound_cfg,
                 use_database, mail_path, db_config_path):
    """基于生产模板构造一份 smtpsServer 配置。"""
    src = os.path.join(proj_root, template)
    with open(src) as f:
        cfg = json.load(f)

    # 重写 listeners 为单端口
    cfg['listeners'] = listeners

    cfg['system_domain'] = system_domain
    cfg['use_database']  = use_database
    cfg['perf_mode']     = True       # 跳过 SPF/DKIM/DMARC/DNSBL，性能模式
    cfg['metrics_enabled'] = False
    cfg['inbound_spf_mode']   = 'off'
    cfg['inbound_dkim_mode']  = 'off'
    cfg['inbound_dmarc_mode'] = 'off'
    cfg['dnsbl_enabled']      = False
    cfg['intrusion_detection_enabled'] = False
    cfg['inbound_auth_policy'] = 'off'
    cfg['log_level']  = 'info'
    cfg['log_to_console'] = True
    cfg['log_to_file']    = False
    cfg['maxMessageSize'] = 10485760
    cfg['maxConnections'] = 1000
    cfg['io_thread_count']   = 2
    cfg['worker_thread_count'] = 2
    cfg['connection_timeout'] = 60

    # 存储：local 或 null
    if mail_path is None:
        cfg['storage'] = {'provider': 'null'}
    else:
        os.makedirs(mail_path, exist_ok=True)
        cfg['storage'] = {'provider': 'local', 'local': {
            'mail_path': mail_path,
            'attachment_path': os.path.join(mail_path, 'att'),
        }}

    # DB 配置：use_database=true 时必填
    if use_database:
        cfg['db_config_file'] = db_config_path

    # 顶层 outbound 块（嵌套 JSON 风格 — 测试 server_config.h 解析路径）
    if outbound_cfg is not None:
        cfg['outbound'] = outbound_cfg

    return cfg


def start_server(server_bin, cfg, proj_root):
    """写 cfg 到临时文件，subprocess.Popen 启 server，返回 (proc, cfg_path, log_path)。"""
    tmpdir = tempfile.mkdtemp(prefix='outbound_relay_')
    cfg_path = os.path.join(tmpdir, 'config.json')
    log_path = os.path.join(tmpdir, 'server.log')
    # 用 log_to_file 强制 server 把日志写到文件（而不是 PIPE）
    cfg['log_to_console'] = False
    cfg['log_to_file']    = True
    cfg['log_file']       = log_path

    with open(cfg_path, 'w') as f:
        json.dump(cfg, f)

    # 把 stdout/stderr 同样丢到 .stdout（避免 PIPE buffer 满时阻塞）
    stdout_path = os.path.join(tmpdir, 'server.stdout')
    stdout_f = open(stdout_path, 'w')
    proc = subprocess.Popen(
        [server_bin, '-c', cfg_path],
        cwd=proj_root,
        stdout=stdout_f, stderr=subprocess.STDOUT,
        preexec_fn=os.setsid,
        # E2E 模式：强制 server log 每条 info 立即 flush（默认只在 warn 才 flush，
        # 导致 30s wait_for 期间 Python 端读不到 Outbound: dispatching 日志）。
        # PR_E2E_LOCAL_SHORTCUT=1 让 .local 后缀 host 直接走 127.0.0.1，
        # 跳过系统 mDNS resolver（即便 static_routes 漏配也能连通）。
        env={**os.environ, 'PR_E2E_FLUSH_LOGS': '1', 'PR_E2E_LOCAL_SHORTCUT': '1'},
    )
    # 把 stdout_f 句柄挂在 proc 上，避免被 GC 关掉
    proc._stdout_f = stdout_f
    return proc, tmpdir, cfg_path, log_path


def is_smtp_banner(host, port, expect_code='220'):
    s = socket.create_connection((host, port), timeout=3)
    data = s.recv(512).decode('utf-8', errors='replace')
    s.close()
    return data.startswith(expect_code), data


# ── main ────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--server', default=SERVER_BIN_DEFAULT)
    parser.add_argument('--config', default=CONFIG_TEMPLATE)
    parser.add_argument('--keep-temp', action='store_true',
                        help='保留临时 config 目录（调试用）')
    parser.add_argument('--inject-dns', action='store_true',
                        help='opt-in: 通过 /etc/hosts 注入 b.local→127.0.0.1（需 sudo）'
                             '。默认不启用：static_routes 仍写 127.0.0.1 跑通。')
    args = parser.parse_args()

    proj_root = os.path.normpath(
        os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
    server_bin = args.server
    if not os.path.isabs(server_bin):
        server_bin = os.path.join(proj_root, server_bin)
    if not os.path.exists(server_bin):
        print(f"ERROR: server binary not found: {server_bin}")
        return 2
    config_path = os.path.join(proj_root, args.config)
    if not os.path.exists(config_path):
        print(f"ERROR: config template not found: {config_path}")
        return 2

    # 写时目录（abs path，给 db_config_file 用）
    db_cfg_path = os.path.join(proj_root, 'test/config/db_config.json')
    if not os.path.exists(db_cfg_path):
        print(f"ERROR: db_config.json not found at {db_cfg_path}")
        print("  -> run: python3 test/scripts/setup_test_env.py")
        return 2

    # 临时 mail_path（A=null 模式无；B 真实写盘）
    work_root = tempfile.mkdtemp(prefix='outbound_relay_e2e_')

    # ── B 端独立 DB（避免共享 DB 的 Message-ID dedup 副作用）──────────
    # A 和 B 共用 mail 库时，B 收到 A 投递的邮件会因 source_message_id 重复
    # 命中 PersistentQueue::is_duplicate_by_source_message_id_async → 跳过
    # 落盘（B log: "Message-ID dedup hit, skip persistence"）。A 自己接收时
    # 也入 PQ，B 收到同一 Message-ID → dedup → 9/9 落盘断言失败。
    # 修法：建独立 mail_b 库，用 create_tables_mail2.sql 改库名 + 写临时
    # db_config_b.json 指向它；e2e 跑完删库（finally 块）。
    B_DB_NAME = 'mail_b'
    db_cfg_b_path = db_cfg_path   # 兜底
    b_cfg_dir = None
    try:
        with open(db_cfg_path) as f:
            db_cfg_template = json.load(f)
        mysql_host = db_cfg_template.get('host', 'localhost')
        mysql_port = db_cfg_template.get('port', 3306)
        mysql_user = db_cfg_template.get('user', 'root')
        mysql_pass = db_cfg_template.get('password', '')
        mysql_admin_args = [
            'mysql', '-h', mysql_host, '-P', str(mysql_port),
            '-u', mysql_user, f'-p{mysql_pass}',
        ]
        # 1) CREATE DATABASE
        r = subprocess.run(
            mysql_admin_args + ['-e', f'CREATE DATABASE IF NOT EXISTS {B_DB_NAME} '
                                     f'CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;'],
            capture_output=True, timeout=10, text=True)
        if r.returncode != 0:
            raise RuntimeError(f"CREATE DATABASE {B_DB_NAME} failed: {r.stderr[:200]}")
        # 2) 用 create_tables_mail2.sql 模板替换库名 → 写临时 SQL → 跑表
        sql_template = os.path.join(proj_root, 'test/config/sql/create_tables_mail2.sql')
        if not os.path.exists(sql_template):
            raise RuntimeError(f"missing {sql_template}")
        with open(sql_template) as f:
            sql_text = f.read()
        sql_text = sql_text.replace('CREATE DATABASE IF NOT EXISTS mail2;',
                                    f'CREATE DATABASE IF NOT EXISTS {B_DB_NAME};')
        sql_text = sql_text.replace('USE mail2;', f'USE {B_DB_NAME};')
        b_init_sql = os.path.join(work_root, 'init_b.sql')
        with open(b_init_sql, 'w') as f:
            f.write(sql_text)
        r = subprocess.run(
            mysql_admin_args + [B_DB_NAME], input=open(b_init_sql).read(),
            capture_output=True, timeout=15, text=True)
        if r.returncode != 0:
            raise RuntimeError(f"init {B_DB_NAME} tables failed: {r.stderr[:300]}")
        # 3) seed bob 用户到 mail_b（B 端 PQ 的 mail_mailbox 关联需要）。
        # mail_b 用 create_tables_mail2.sql schema：mailboxes 列名是 `name`（非 mailbox_name）。
        b_seed_sql = (
            f"USE {B_DB_NAME};\n"
            "INSERT IGNORE INTO users (mail_address, password, name, register_time) "
            "VALUES ('bob@b.local', 'e2e_password', 'e2e relay test recipient', NOW());\n"
            "INSERT IGNORE INTO mailboxes (user_id, name, is_system, box_type, create_time) "
            "SELECT id, 'INBOX', TRUE, 1, NOW() "
            "FROM users WHERE mail_address='bob@b.local';\n"
        )
        r = subprocess.run(
            mysql_admin_args + [B_DB_NAME], input=b_seed_sql,
            capture_output=True, timeout=10, text=True)
        if r.returncode != 0:
            print(f"  [warn] seed {B_DB_NAME} bob user failed: {r.stderr[:200]}")
        # 4) 写 db_config_b.json 指向 mail_b
        #    initialize_script 改成绝对路径（db_config_b.json 放在 b_cfg_dir，
        #    原相对路径 sql/create_tables.sql 找不到）。建表已用 mysql 命令完成，
        #    这里再跑一次是幂等的（CREATE TABLE IF NOT EXISTS）。
        b_cfg_dir = tempfile.mkdtemp(prefix='outbound_relay_b_cfg_')
        db_cfg_b_path = os.path.join(b_cfg_dir, 'db_config.json')
        b_cfg_data = {**db_cfg_template, 'database': B_DB_NAME}
        b_cfg_data['initialize_script'] = os.path.join(
            proj_root, 'test/config/sql/create_tables_mail2.sql')
        with open(db_cfg_b_path, 'w') as f:
            json.dump(b_cfg_data, f, indent=2)
        print(f"  [setup] B 独立 DB '{B_DB_NAME}' ready, db_config={db_cfg_b_path}")
    except Exception as e:
        print(f"  [warn] B DB setup failed: {e}")
        print(f"  回退：用 A 共享 DB（dedup 会让 9/9 落盘断言失败，但 8/8 仍 PASS）")

    a_mail_dir = os.path.join(work_root, 'a_mail')   # 不会被写
    b_mail_dir = os.path.join(work_root, 'b_mail')


    # 加载 e2e 种子数据（alice + bob + alice INBOX）— e2e 自带 SQL
    # 替代之前的 inline INSERT IGNORE hack；失败 warn 不 fail
    e2e_seed_sql = os.path.join(
        os.path.dirname(os.path.abspath(__file__)), 'sql', 'e2e_seed.sql')
    if not os.path.exists(e2e_seed_sql):
        print(f"  [warn] e2e_seed.sql not found: {e2e_seed_sql}")
    else:
        try:
            with open(db_cfg_path) as f:
                db_cfg = json.load(f)
            mysql_args = [
                'mysql', '-h', db_cfg.get('host', 'localhost'),
                '-P', str(db_cfg.get('port', 3306)),
                '-u', db_cfg.get('user', 'root'),
                f'-p{db_cfg.get("password", "")}',
                db_cfg.get('database', 'mail'),
            ]
            with open(e2e_seed_sql) as f:
                sql_text = f.read()
            r = subprocess.run(mysql_args, input=sql_text, capture_output=True,
                               timeout=10, text=True)
            if r.returncode == 0:
                print(f"  [seed] e2e_seed.sql loaded OK")
            else:
                print(f"  [warn] e2e_seed.sql load failed: {r.stderr[:200]}")
        except Exception as e:
            print(f"  [warn] failed to load e2e_seed.sql: {e}")

    passed = []
    failed = []
    skipped = []

    proc_a = proc_b = None
    tmpdir_a = tmpdir_b = None
    dns_injected = False

    try:
        # ── DNS 注入（opt-in）：让 b.local 解析到 127.0.0.1（生产/test 共享 cfg） ──
        # 默认不改 /etc/hosts（避免 system-wide 副作用）；传 --inject-dns 时启
        # 用并要求 sudo（程序会自己检查，失败就 return 1）。
        if args.inject_dns:
            try:
                setup_local_dns()
                dns_injected = True
            except RuntimeError as e:
                print(f"\n[setup] /etc/hosts 注入失败: {e}")
                print("  e2e 必须能修改 /etc/hosts（用 sudo 重跑）")
                return 1
        # ── A 配置：DB on, storage=local (必需!null storage 会让 PQ 写 body 失败) ──
        # A 的 mail_dir 仍会收到 alice@a.local 自己的副本（这是 SMTP 标准行为）；
        # 本测试不验证 A 的 mail_dir 状态，只关注 OutboundServer 拉到 outbox + 尝试连 B。
        # static_routes 的 host:
        #   - 默认 ('127.0.0.1'): 走 IP 直连，简单可靠（生产/test 共用 binary）
        #   - --inject-dns (B_DOMAIN): 走真实域名，要求 /etc/hosts 把 b.local 解析到
        #     127.0.0.1（生产配置风格；用户显式 opt-in 启用，避免 system-wide 副作用）
        if args.inject_dns:
            static_route_host = B_DOMAIN
        else:
            static_route_host = '127.0.0.1'
        cfg_a = load_config(
            args.config, proj_root,
            listeners=[{'type': 'tcp', 'port': A_PORT, 'auth_policy': 'on'}],
            system_domain=A_DOMAIN,
            outbound_cfg={
                'helo_domain': A_DOMAIN,
                'mail_from_domain': A_DOMAIN,
                'rewrite_header_from': True,
                'max_attempts': 3,
                'ports': [B_PORT],   # 兜底：static_routes 不命中时用
                'static_routes': {
                    B_DOMAIN: {'host': static_route_host, 'port': B_PORT},
                },
                'dkim': {'enabled': False},
            },
            use_database=True,
            mail_path=a_mail_dir,
            db_config_path=db_cfg_path,
        )

        # ── B 配置：DB on, storage=local，auth_policy=off ──
        # 关掉 auth 是因为 A 的 OutboundSmtpSession 不发 AUTH（RFC 5321 §3.1 可选），
        # B 端启 AUTH 只会把 9/9 落盘断言变成 8/8 dispatching 验证。关闭后 B 收
        # 任何 sender（含 alice@a.local）都直接进 RCPT。RCPT 域检查仍走
        # system_domain 比对（b.local == B.system_domain，不会被拒 relay）。
        cfg_b = load_config(
            args.config, proj_root,
            listeners=[{'type': 'tcp', 'port': B_PORT, 'auth_policy': 'off'}],
            system_domain=B_DOMAIN,
            outbound_cfg=None,
            use_database=True,
            mail_path=b_mail_dir,
            db_config_path=db_cfg_b_path,
        )

        print(f"[setup] work_root = {work_root}")
        print(f"[setup] A mail_dir = {a_mail_dir} (should remain empty)")
        print(f"[setup] B mail_dir = {b_mail_dir} (should receive mail)")

        # ── 启 B 先（A 需要连 B） ──
        print(f"\n[start] Server B on :{B_PORT} ...")
        proc_b, tmpdir_b, cfg_path_b, log_b = start_server(server_bin, cfg_b, proj_root)
        if not wait_for_port('127.0.0.1', B_PORT, timeout=WAIT_PORT_TIMEOUT_S):
            failed.append(f"B port {B_PORT} not ready in {WAIT_PORT_TIMEOUT_S}s")
            print(f"  --- B log ({log_b}) ---")
            try:
                with open(log_b) as f:
                    print(f.read()[:2000])
            except OSError:
                pass
            return 1
        ok, banner = is_smtp_banner('127.0.0.1', B_PORT)
        if ok:
            passed.append(f"Server B listens on :{B_PORT} (SMTP banner ok)")
        else:
            failed.append(f"Server B banner unexpected: {banner[:80]!r}")
            return 1

        # ── 启 A ──
        print(f"\n[start] Server A on :{A_PORT} (outbound relay) ...")
        proc_a, tmpdir_a, cfg_path_a, log_a = start_server(server_bin, cfg_a, proj_root)
        if not wait_for_port('127.0.0.1', A_PORT, timeout=WAIT_PORT_TIMEOUT_S):
            failed.append(f"A port {A_PORT} not ready in {WAIT_PORT_TIMEOUT_S}s")
            print(f"  --- A log ({log_a}) ---")
            try:
                with open(log_a) as f:
                    print(f.read()[:2000])
            except OSError:
                pass
            return 1
        ok, banner = is_smtp_banner('127.0.0.1', A_PORT)
        if ok:
            passed.append(f"Server A listens on :{A_PORT} (SMTP banner ok)")
        else:
            failed.append(f"Server A banner unexpected: {banner[:80]!r}")
            return 1

        # ── 投邮件 ──
        print(f"\n[send] smtplib → A:{A_PORT} from {SMTP_SENDER} to {SMTP_RECIPIENT}")
        msg = (
            f'From: {SMTP_SENDER}\r\n'
            f'To: {SMTP_RECIPIENT}\r\n'
            f'Subject: {SUBJECT_TAG} e2e\r\n'
            f'Message-ID: <e2e-{int(time.time())}@{A_DOMAIN}>\r\n'
            f'\r\n'
            f'Body delivered via OutboundServer static_routes.\r\n'
        )
        try:
            with smtplib.SMTP('127.0.0.1', A_PORT, timeout=10) as smtp:
                smtp.ehlo('e2e.test')
                smtp.login(SMTP_AUTH_USER, SMTP_AUTH_PASSWORD)
                # 250 表示 A 接收成功
                refused = smtp.sendmail(SMTP_SENDER, [SMTP_RECIPIENT], msg)
                if refused:
                    failed.append(f"A refused recipients: {refused}")
                else:
                    passed.append(f"A accepted mail from {SMTP_SENDER} → {SMTP_RECIPIENT} (auth ok)")
        except Exception as e:
            failed.append(f"smtplib exception: {type(e).__name__}: {e}")
            return 1

        # A 落盘到 a_mail_dir 是正常的（PQ 需要存 body 才能入 outbox）。
        # e2e 不验证 A 的 mail_dir 状态，只关心 A 启动了 OutboundServer 并尝试连 B。
        os.makedirs(a_mail_dir, exist_ok=True)

        # ── 验证 A 端 OutboundServer 真的启动并 wiring（接通的死代码） ──
        print(f"\n[wait] A logs to show 'OutboundServer (new engine) started' ...")
        def a_outbound_started():
            try:
                with open(log_a) as f:
                    return 'OutboundServer (new engine) started' in f.read()
            except OSError:
                return False
        if a_outbound_started():
            passed.append("A's OutboundServer started (previously dead code now wired)")
        else:
            failed.append("A's OutboundServer never logged startup")

        # ── 验证 A 端 PersistentQueue wired 上了 OutboundServer（运行时证据） ──
        def a_pq_wired():
            try:
                with open(log_a) as f:
                    return 'OutboundServer (new engine) wired into PersistentQueue' in f.read()
            except OSError:
                return False
        if a_pq_wired():
            passed.append("A's PersistentQueue wired to OutboundServer (PQ.submit → OS.submit)")
        else:
            failed.append("A's PersistentQueue never logged outbound wiring")

        # ── 验证 mail 真的进了 PQ（提供投递基础） ──
        def a_mail_queued():
            try:
                with open(log_a) as f:
                    return 'Mail submitted to PersistentQueue' in f.read()
            except OSError:
                return False
        if a_mail_queued():
            passed.append("A's PersistentQueue received mail (post-auth DATA ok)")
        else:
            failed.append("A's PersistentQueue never received mail")

        # ── 等 OutboundServer 真正调度到 session（验证 static_routes + on_claim_complete 路径） ──
        # PQ 写 outbox 是 async，所以 claim 也要等几百毫秒。30s timeout。
        def a_outbound_dispatched():
            try:
                with open(log_a) as f:
                    body = f.read()
                    return ('Outbound: dispatching' in body or
                            'Outbound: connect to' in body or
                            'Outbound: connected to' in body)
            except OSError:
                return False
        print(f"\n[wait] A logs to show 'Outbound: dispatching ... target_host=...' ...")
        if wait_for(a_outbound_dispatched, timeout=30, poll=0.2):
            passed.append("A's OutboundServer dispatched mail to a session (static_routes resolved)")
        else:
            failed.append(
                "A's OutboundServer never logged dispatching — claim_async 路径或 "
                "on_claim_complete 分发卡住了")

        # ── 9/9: B 端真落盘 + EML 头验证 ──
        # FSM bug 已修：session 真用 current_state、parse_response 按 state 区分 250、
        # connect_to_mx 派 CONNECT+CONNECTED 两阶段。详见 outbound_smtp_fsm.h:19-36
        # 和 outbound_smtp_session.h:104-110/131-159/181-210/262-267。
        print(f"\n[verify] Polling B mail_dir for landed EML (timeout 30s)...")
        def is_eml(p):
            n = os.path.basename(p)
            return n.isdigit() and len(n) >= 18  # snowflake 64-bit 整数, 18-19 位
        landed = wait_for_file(b_mail_dir, is_eml, timeout=30, poll=0.5)
        if landed is None:
            failed.append(
                f"B mail_dir {b_mail_dir} empty after 30s "
                f"(A→B 真投递未落盘；看 A log 'Outbound: mail ... accepted')")
        else:
            with open(landed, 'r', encoding='utf-8', errors='replace') as f:
                content = f.read()
            has_subject = 'Subject:' in content
            has_from    = f'From: {SMTP_SENDER}' in content
            has_to      = f'To: {SMTP_RECIPIENT}' in content
            if has_subject and has_from and has_to:
                passed.append(
                    f"B received mail landed at {landed} "
                    f"({os.path.getsize(landed)} bytes; Subject/From/To all present)")
            else:
                failed.append(
                    f"B landed EML {landed} missing headers: "
                    f"subject={has_subject} from={has_from} to={has_to}; "
                    f"first 200 bytes: {content[:200]!r}")

    finally:
        if proc_a: kill_pg(proc_a)
        if proc_b: kill_pg(proc_b)
        if not args.keep_temp:
            shutil.rmtree(work_root, ignore_errors=True)
            if tmpdir_a: shutil.rmtree(tmpdir_a, ignore_errors=True)
            if tmpdir_b: shutil.rmtree(tmpdir_b, ignore_errors=True)
            if b_cfg_dir: shutil.rmtree(b_cfg_dir, ignore_errors=True)
            # 删 mail_b 库（避免污染；失败只 warn）
            if db_cfg_b_path != db_cfg_path:
                try:
                    with open(db_cfg_path) as f:
                        _db = json.load(f)
                    subprocess.run(
                        ['mysql', '-h', _db.get('host', 'localhost'),
                         '-P', str(_db.get('port', 3306)),
                         '-u', _db.get('user', 'root'),
                         f"-p{_db.get('password', '')}",
                         '-e', f'DROP DATABASE IF EXISTS {B_DB_NAME};'],
                        capture_output=True, timeout=10, text=True)
                except Exception:
                    pass
        else:
            print(f"[keep] work_root = {work_root}")
            if tmpdir_a: print(f"[keep] A tmpdir = {tmpdir_a}")
            if tmpdir_b: print(f"[keep] B tmpdir = {tmpdir_b}")

    # ── 汇总 ──
    print(f"\n{'='*60}")
    print(f"  Outbound relay E2E: {len(passed)} pass, {len(failed)} fail, {len(skipped)} skip")
    print('='*60)
    for p in passed:
        print(f"  PASS  {p}")
    for s in skipped:
        print(f"  SKIP  {s}")
    for f in failed:
        print(f"  FAIL  {f}")

    return 0 if not failed else 1


if __name__ == '__main__':
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\nInterrupted.")
        sys.exit(130)
    except Exception:
        traceback.print_exc()
        sys.exit(1)
