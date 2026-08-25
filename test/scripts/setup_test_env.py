#!/usr/bin/env python3
"""
测试环境初始化 — 拷贝证书/SQL/DKIM，生成 mock 和 real 两套配置。

生成文件:
  test/config/
    ├── crt/                    # 从 config/crt/ 拷贝
    ├── dkim/                   # 从 config/dkim/ 拷贝
    ├── create_tables.sql       # 从 config/sql/ 拷贝
    ├── db_config.json          # 数据库配置
    ├── router_config.json      # 分片配置
    ├── smtps_mock.json         # mock 模式: use_database=false
    ├── imaps_mock.json         # mock 模式: use_database=false
    ├── smtps_real.json         # real 模式: use_database=true
    └── imaps_real.json         # real 模式: use_database=true

运行时目录（/tmp/protorelay_test/，重启自动清除）:
    ├── mail/                   # 邮件落盘
    ├── attachments/            # 附件落盘
    └── logs/                   # 日志

用法:
  python3 test/scripts/setup_test_env.py          # 生成所有
  python3 test/scripts/setup_test_env.py --clean  # 清理
"""
import json
import os
import shutil
import sys
import argparse

PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TEST_DIR    = os.path.join(PROJECT_DIR, "test")
CONFIG_DIR  = os.path.join(TEST_DIR, "config")
TMP_ROOT    = "/tmp/protorelay_test"

# ── 端口（高端口避免 sudo）────
SMTP_PORTS = [
    {"type": "tcp", "port": 2525, "auth_policy": "off"},
    {"type": "ssl", "port": 8465, "auth_policy": "on"},
    {"type": "tcp", "port": 8587, "auth_policy": "on"},
]
IMAP_PORTS = [
    {"type": "tcp", "port": 1414, "auth_policy": "off"},
    {"type": "ssl", "port": 8993, "auth_policy": "on"},
]

# ── 测试账号（硬编码，不会删）────
TEST_USER     = "test2@test.local"
TEST_PASSWORD = "test123"


def copy_dir(src, dst):
    """拷贝目录（覆盖已有）。"""
    if os.path.isdir(dst):
        shutil.rmtree(dst)
    if os.path.isdir(src):
        shutil.copytree(src, dst)
        return True
    return False


def copy_file(src, dst):
    """拷贝文件。"""
    if os.path.exists(src):
        shutil.copy2(src, dst)
        return True
    return False


def apply_common(cfg, mode, config_type):
    """应用公共修改：端口、证书路径、存储路径。"""
    if config_type == "smtp":
        cfg["listeners"] = SMTP_PORTS
    else:
        cfg["listeners"] = IMAP_PORTS

    # 证书指向 test/config/crt/
    for key in ["certFile", "keyFile"]:
        val = cfg.get(key, "")
        if val and not os.path.isabs(val):
            # 原值如 "crt/server.crt" → test/config/crt/server.crt
            cfg[key] = os.path.join(CONFIG_DIR, val.replace("config/", ""))

    # DKIM 私钥
    if "outbound" in cfg and "dkim" in cfg["outbound"]:
        pk = cfg["outbound"]["dkim"].get("private_key_file", "")
        if pk and not os.path.isabs(pk):
            cfg["outbound"]["dkim"]["private_key_file"] = os.path.join(
                CONFIG_DIR, pk.replace("config/", ""))

    # 存储 → /tmp/protorelay_test/
    if "storage" in cfg and "local" in cfg.get("storage", {}):
        cfg["storage"]["local"]["mail_path"]       = f"{TMP_ROOT}/mail"
        cfg["storage"]["local"]["attachment_path"] = f"{TMP_ROOT}/attachments"

    # 日志
    cfg["log_to_console"] = True
    cfg["log_to_file"] = False
    cfg["log_file"] = f"{TMP_ROOT}/logs/server.log"

    # 通用
    cfg["dnsbl_enabled"] = False
    cfg["intrusion_detection_enabled"] = False
    cfg["metrics_enabled"] = False
    cfg["perf_mode"] = True
    cfg["inbound_spf_mode"] = "off"
    cfg["inbound_dkim_mode"] = "off"
    cfg["inbound_dmarc_mode"] = "off"


def make_mock(cfg, config_type):
    """mock 模式：无 DB。"""
    cfg["use_database"] = False
    apply_common(cfg, "mock", config_type)


def make_real(cfg, config_type):
    """real 模式：连接真实 DB。"""
    cfg["use_database"] = True
    cfg["db_config_file"] = os.path.join(CONFIG_DIR, "db_config.json")
    apply_common(cfg, "real", config_type)


def gen_config(src_name, dst_name, modifier):
    """从 config/ 读取源配置，应用修改，写入 test/config/。"""
    src = os.path.join(PROJECT_DIR, "config", src_name)
    dst = os.path.join(CONFIG_DIR, dst_name)
    if not os.path.exists(src):
        print(f"  [SKIP] {src_name} not found")
        return
    with open(src) as f:
        cfg = json.load(f)
    modifier(cfg)
    with open(dst, "w") as f:
        json.dump(cfg, f, indent=2)
    print(f"  [OK] {dst_name}")


def setup():
    os.makedirs(CONFIG_DIR, exist_ok=True)

    # 1. 创建 /tmp 运行时目录
    for sub in ["mail", "attachments", "logs"]:
        os.makedirs(f"{TMP_ROOT}/{sub}", exist_ok=True)

    # 2. 拷贝资源
    print("[COPY]")
    # sql/ → sql/ （db_config.json 期望的 initialize_script 路径）
    # sql/ → sql_init/ （旧路径，保留兼容）
    for src_sub, dst_sub in [("crt", "crt"), ("dkim", "dkim"),
                              ("sql", "sql"), ("sql", "sql_init")]:
        src = os.path.join(PROJECT_DIR, "config", src_sub)
        dst = os.path.join(CONFIG_DIR, dst_sub)
        if copy_dir(src, dst):
            print(f"  config/{src_sub}/ → test/config/{dst_sub}/")

    # SQL 文件
    sql_src = os.path.join(PROJECT_DIR, "config", "sql", "create_tables.sql")
    sql_dst = os.path.join(CONFIG_DIR, "create_tables.sql")
    if copy_file(sql_src, sql_dst):
        print(f"  config/sql/create_tables.sql → test/config/")

    # DB / router 配置
    for name in ["db_config.json", "router_config.json"]:
        if copy_file(os.path.join(PROJECT_DIR, "config", name),
                     os.path.join(CONFIG_DIR, name)):
            print(f"  config/{name} → test/config/")

    # 3. 生成配置
    print("\n[GEN] Mock 配置（无 DB）")
    gen_config("smtpsConfig.json", "smtps_mock.json", lambda c: make_mock(c, "smtp"))
    gen_config("imapsConfig.json", "imaps_mock.json", lambda c: make_mock(c, "imap"))

    print("\n[GEN] Real 配置（真实 DB）")
    gen_config("smtpsConfig.json", "smtps_real.json", lambda c: make_real(c, "smtp"))
    gen_config("imapsConfig.json", "imaps_real.json", lambda c: make_real(c, "imap"))

    # 4. 清理脚本
    cleanup_sh = os.path.join(TEST_DIR, "scripts", "cleanup_test_env.sh")
    with open(cleanup_sh, "w") as f:
        f.write("#!/bin/bash\nset -e\n")
        f.write("# 清理测试环境\n")
        f.write(f'rm -rf "{TMP_ROOT}"\n')
        f.write(f'rm -rf "{CONFIG_DIR}/crt" "{CONFIG_DIR}/dkim" "{CONFIG_DIR}/sql_init" "{CONFIG_DIR}/sql"\n')
        f.write(f'rm -f "{CONFIG_DIR}/create_tables.sql"\n')
        f.write(f'rm -f "{CONFIG_DIR}/smtps_mock.json" "{CONFIG_DIR}/smtps_real.json"\n')
        f.write(f'rm -f "{CONFIG_DIR}/imaps_mock.json" "{CONFIG_DIR}/imaps_real.json"\n')
        f.write(f'rm -f "{CONFIG_DIR}/db_config.json" "{CONFIG_DIR}/router_config.json"\n')
        f.write('pkill -f "smtpsServer.*test/config" 2>/dev/null || true\n')
        f.write('pkill -f "imapsServer.*test/config" 2>/dev/null || true\n')
        f.write('echo "Test environment cleaned."\n')
    os.chmod(cleanup_sh, 0o755)
    print(f"\n[OK] test/scripts/cleanup_test_env.sh")

    # 5. 摘要
    print(f"""
=== 测试环境就绪 ===
  配置目录: {CONFIG_DIR}
  Mock 模式: smtps_mock.json / imaps_mock.json (无 DB)
  Real 模式: smtps_real.json / imaps_real.json (真实 DB)
  运行时目录: {TMP_ROOT} (重启自动清除)
  测试账号: {TEST_USER} / {TEST_PASSWORD}

启动 Mock:
  ./build/smtpsServer -c test/config/smtps_mock.json
启动 Real:
  ./build/smtpsServer -c test/config/smtps_real.json
""")


def clean():
    """清理测试环境。"""
    import subprocess
    subprocess.run(["pkill", "-f", "smtpsServer.*test/config"], capture_output=True)
    subprocess.run(["pkill", "-f", "imapsServer.*test/config"], capture_output=True)
    for d in [f"{TMP_ROOT}", f"{CONFIG_DIR}/crt", f"{CONFIG_DIR}/dkim",
              f"{CONFIG_DIR}/sql_init"]:
        if os.path.exists(d):
            shutil.rmtree(d, ignore_errors=True)
    for f in ["create_tables.sql", "smtps_mock.json", "smtps_real.json",
              "imaps_mock.json", "imaps_real.json", "db_config.json",
              "router_config.json"]:
        path = os.path.join(CONFIG_DIR, f)
        if os.path.exists(path):
            os.remove(path)
    print("Test environment cleaned.")


if __name__ == "__main__":
    p = argparse.ArgumentParser(description="ProtoRelay 测试环境初始化")
    p.add_argument("--clean", action="store_true", help="清理测试环境")
    args = p.parse_args()

    if args.clean:
        clean()
    else:
        setup()
