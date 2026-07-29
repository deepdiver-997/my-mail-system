#!/usr/bin/env python3
"""
SMTP stress tester — 正交维度：连接复用 × 流水线 × TLS

  --mode mta-relay     port 25, no auth, no TLS (传统 MTA)
  --mode submission    port 587, STARTTLS, AUTH (客户端提交)
  --mode smtps         port 465, implicit TLS, AUTH (SMTPS)

  正交参数 (可覆盖 mode 默认值):
  --pipeline           batch all commands in one TCP write (default: false)
  --reuse-conn         reuse TCP connection for many messages (default: false)
  --tls none|starttls|implicit  (default: mode-dependent)

Usage:
  # MTA relay, pipeline, per-conn (max throughput)
  python cl.py --mode mta-relay --pipeline

  # MTA relay, sequential, connection reuse (traditional MTA)
  python cl.py --mode mta-relay --reuse-conn

  # Client submission, pipeline, per-conn (realistic TLS client)
  python cl.py --mode submission --pipeline -u user -p pass

  # Full scan
  python cl.py --scan --messages 5000
"""

import argparse, base64, random, time, threading, smtplib, socket, ssl, sys

SENDERS    = [f"user{i}@scut.email" for i in range(10)]
RECIPIENTS = [f"dest{i}@scut.email" for i in range(10)]
MSG_BODY   = "From: {sender}\r\nTo: {rcpt}\r\nSubject: perf\r\n\r\nhi\r\n"

MODE_DEFAULTS = {
    "mta-relay":  {"port": 25,  "tls": "none",     "auth": False},
    "submission": {"port": 587, "tls": "starttls",  "auth": True},
    "smtps":      {"port": 465, "tls": "implicit",  "auth": True},
}


# ── helpers ────────────────────────────────────────────────────────────────
def _shutdown_tls(sock):
    if sock is None: return
    try: sock.shutdown(socket.SHUT_RDWR)
    except: pass
    try: sock.close()
    except: pass


def _read_line(f):
    line = f.readline()
    if not line: raise ConnectionError("closed")
    return line.decode("utf-8", errors="replace") if isinstance(line, bytes) else line


def _read_multiline(f):
    while True:
        s = _read_line(f)
        if len(s) >= 4 and s[3] == " ": return s


# ── strategy: sequential (smtplib) ─────────────────────────────────────────
def _make_client(args):
    tls = args.tls
    timeout = args.timeout
    if tls == "implicit":
        ctx = ssl.create_default_context()
        ctx.check_hostname = False; ctx.verify_mode = ssl.CERT_NONE
        sock = ctx.wrap_socket(socket.socket()); sock.settimeout(timeout)
        sock.connect((args.host, args.port))
        c = smtplib.SMTP(); c.sock = sock; c.file = sock.makefile("rb")
    else:
        c = smtplib.SMTP(args.host, args.port, timeout=timeout)
        if tls == "starttls": c.starttls()
    if args.user: c.login(args.user, args.password)
    return c


def _worker_seq_reuse(idx, args, stats, lats):
    """Sequential commands, one connection per thread (reused)."""
    n = args.messages // args.concurrency + (1 if idx < args.messages % args.concurrency else 0)
    if n == 0: return
    try:
        c = _make_client(args)
        for _ in range(n):
            s = random.choice(SENDERS); r = random.choice(RECIPIENTS)
            t0 = time.perf_counter()
            c.sendmail(s, [r], MSG_BODY.format(sender=s, rcpt=r))
            stats["ok"] += 1
            lats.append((time.perf_counter() - t0) * 1000)
        c.quit()
    except Exception as e:
        stats["fail"] += n
        if args.verbose: print(f"  [w{idx}] seq-reuse error: {e}")


def _worker_seq_perconn(idx, args, stats, lats):
    """Sequential commands, new connection per message."""
    n = args.messages // args.concurrency + (1 if idx < args.messages % args.concurrency else 0)
    for _ in range(n):
        t0 = time.perf_counter()
        try:
            c = _make_client(args)
            s = random.choice(SENDERS); r = random.choice(RECIPIENTS)
            c.sendmail(s, [r], MSG_BODY.format(sender=s, rcpt=r))
            c.quit()
            stats["ok"] += 1
            lats.append((time.perf_counter() - t0) * 1000)
        except Exception as e:
            stats["fail"] += 1
            if args.verbose: print(f"  [w{idx}] seq-perconn error: {e}")


# ── strategy: pipeline (raw socket, batched write) ─────────────────────────
def _pipeline_plain(args, sock, f):
    """Pipeline on already-opened plain connection."""
    s = random.choice(SENDERS); r = random.choice(RECIPIENTS)
    body = MSG_BODY.format(sender=s, rcpt=r)
    cmds = (f"EHLO t\r\nMAIL FROM:<{s}>\r\nRCPT TO:<{r}>\r\n"
            f"DATA\r\n{body}\r\n.\r\nQUIT\r\n")
    sock.sendall(cmds.encode())
    _read_multiline(f)                        # EHLO
    for expect in ("250", "250", "354", "250"):
        resp = _read_line(f)
        if not resp.startswith(expect):
            raise Exception(f"expected {expect}, got {resp.strip()}")


def _pipeline_tls_handshake(args, sock, f):
    """Do TLS handshake + AUTH, return (sock, f) ready for pipelining."""
    tls = args.tls
    if tls == "starttls":
        # plain EHLO → STARTTLS → TLS upgrade
        sock.sendall(b"EHLO t\r\n"); _read_multiline(f)
        sock.sendall(b"STARTTLS\r\n")
        if not _read_line(f).startswith("220"): raise Exception("STARTTLS rejected")
        ctx = ssl.create_default_context(); ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        sock = ctx.wrap_socket(sock); f = sock.makefile("rb")
    elif tls == "implicit":
        sock = ssl.create_default_context().wrap_socket(sock)
        f = sock.makefile("rb")
        if not _read_line(f).startswith("220"): raise Exception("bad banner")
    # RFC 3207: re-EHLO after TLS (both modes)
    sock.sendall(b"EHLO t\r\n"); _read_multiline(f)
    # AUTH LOGIN
    if args.user:
        sock.sendall(b"AUTH LOGIN\r\n")
        if not _read_line(f).startswith("334"): raise Exception("AUTH prompt")
        sock.sendall(base64.b64encode(args.user.encode()) + b"\r\n"); _read_line(f)
        sock.sendall(base64.b64encode(args.password.encode()) + b"\r\n")
        if not _read_line(f).startswith("235"): raise Exception("AUTH failed")
    return sock, f


def _pipeline_one(args, sock, f, need_ehlo=False, do_quit=False):
    """Pipeline one mail on already-authenticated connection."""
    s = random.choice(SENDERS); r = random.choice(RECIPIENTS)
    body = MSG_BODY.format(sender=s, rcpt=r)
    cmds = ""
    if need_ehlo: cmds += "EHLO t\r\n"
    cmds += (f"MAIL FROM:<{s}>\r\nRCPT TO:<{r}>\r\n"
             f"DATA\r\n{body}\r\n.\r\n")
    if do_quit: cmds += "QUIT\r\n"
    sock.sendall(cmds.encode())
    if need_ehlo: _read_multiline(f)
    for expect in ("250", "250", "354", "250"):
        resp = _read_line(f)
        if not resp.startswith(expect):
            raise Exception(f"expected {expect}, got {resp.strip()}")


def _worker_pipe_perconn(idx, args, stats, lats):
    """Pipeline, new connection per message."""
    n = args.messages // args.concurrency + (1 if idx < args.messages % args.concurrency else 0)
    ok = fail = 0; ll = []
    tls = args.tls
    for _ in range(n):
        t0 = time.perf_counter(); sock = None
        try:
            sock = socket.socket(); sock.settimeout(args.timeout)
            sock.connect((args.host, args.port))
            f = sock.makefile("rb")
            if tls == "none":
                if not _read_line(f).startswith("220"): raise Exception("banner")
                _pipeline_plain(args, sock, f)
            else:
                # TLS modes: read banner (implicit TLS already read it in handshake)
                if tls != "implicit":
                    if not _read_line(f).startswith("220"): raise Exception("banner")
                sock, f = _pipeline_tls_handshake(args, sock, f)
                _pipeline_one(args, sock, f)
            ok += 1
            ll.append((time.perf_counter() - t0) * 1000)
        except Exception as e:
            fail += 1
            if args.verbose: print(f"  [w{idx}] pipe-perconn error: {e}")
        finally: _shutdown_tls(sock)
    stats["ok"] += ok; stats["fail"] += fail; lats.extend(ll)


def _worker_pipe_reuse(idx, args, stats, lats):
    """Pipeline, one connection per thread (reused)."""
    n = args.messages // args.concurrency + (1 if idx < args.messages % args.concurrency else 0)
    if n == 0: return
    ok = fail = 0; ll = []; sock = None
    try:
        sock = socket.socket(); sock.settimeout(args.timeout)
        sock.connect((args.host, args.port))
        f = sock.makefile("rb")
        tls = args.tls
        # initial handshake (plain or TLS)
        if tls == "none":
            if not _read_line(f).startswith("220"): raise Exception("banner")
        else:
            if tls != "implicit":
                if not _read_line(f).startswith("220"): raise Exception("banner")
            sock, f = _pipeline_tls_handshake(args, sock, f)
        # 第一条需要 EHLO（server 在 WAIT_EHLO），后续在 WAIT_MAIL_FROM
        for i in range(n):
            t0 = time.perf_counter()
            try:
                need_ehlo = (i == 0 and tls == "none")
                _pipeline_one(args, sock, f, need_ehlo=need_ehlo, do_quit=(i == n - 1))
                ok += 1
                ll.append((time.perf_counter() - t0) * 1000)
            except Exception as e:
                fail += (n - i)
                if args.verbose: print(f"  [w{idx}] pipe-reuse error: {e}")
                break
    except Exception as e:
        fail += n
        if args.verbose: print(f"  [w{idx}] pipe-reuse setup error: {e}")
    finally: _shutdown_tls(sock)
    stats["ok"] += ok; stats["fail"] += fail; lats.extend(ll)


# ── main ───────────────────────────────────────────────────────────────────
def fmt_latency(lats):
    if not lats: return ""
    s = sorted(lats); n = len(s)
    return (f"  p50={s[n//2]:.1f}ms  p99={s[int(n*0.99)]:.1f}ms"
            f"  p999={s[min(n-1,int(n*0.999))]:.1f}ms")


def run_benchmark(args, label=""):
    stats = {"ok": 0, "fail": 0}; lats = []
    if args.pipeline:
        fn = _worker_pipe_reuse if args.reuse_conn else _worker_pipe_perconn
    else:
        fn = _worker_seq_reuse if args.reuse_conn else _worker_seq_perconn

    threads = []; t0 = time.perf_counter()
    for i in range(args.concurrency):
        t = threading.Thread(target=fn, args=(i, args, stats, lats))
        t.start(); threads.append(t)
    for t in threads: t.join()
    elapsed = time.perf_counter() - t0
    ok, fail = stats["ok"], stats["fail"]
    rate = ok / elapsed if elapsed > 0 else 0
    prefix = f"[{label}] " if label else ""
    info = (f"{prefix}total={ok+fail} ok={ok} fail={fail} "
            f"elapsed={elapsed:.1f}s rate={rate:.0f} msg/s")
    if lats: info += fmt_latency(lats)
    print(info)
    return {"ok": ok, "fail": fail, "elapsed": elapsed, "rate": rate}


def run_ramp(args):
    print(f"\n{'='*60}")
    print(f"RAMP: {args.ramp_start}→{args.ramp_end} step={args.ramp_step} "
          f"pipe={args.pipeline} reuse={args.reuse_conn} tls={args.tls}")
    best_c, best_r = 0, 0
    for c in range(args.ramp_start, args.ramp_end + 1, args.ramp_step):
        a = argparse.Namespace(**vars(args)); a.concurrency = c
        r = run_benchmark(a, f"c={c}")
        if r["rate"] > best_r: best_r = r["rate"]; best_c = c
    print(f"Peak: {best_r:.0f} msg/s @ c={best_c}")


def run_scan(args):
    modes  = ["mta-relay"]
    pipes  = [False, True]
    reuses = [False, True]
    print(f"\n{'='*60}")
    print(f"SCAN: {len(modes)}×{len(pipes)}×{len(reuses)} combos, "
          f"{args.messages} msgs, c={args.concurrency}")
    for mode in modes:
        for pipe in pipes:
            for reuse in reuses:
                # skip combos requiring auth without creds
                preset = MODE_DEFAULTS[mode]
                if preset["auth"] and not args.user: continue
                a = argparse.Namespace(**vars(args))
                a.mode = mode; a.pipeline = pipe; a.reuse_conn = reuse
                a.port = preset["port"]; a.tls = preset["tls"]
                if preset["auth"]:
                    a.user = args.user; a.password = args.password
                label = f"{mode} pipe={'Y' if pipe else 'N'} reuse={'Y' if reuse else 'N'}"
                run_benchmark(a, label)


def main():
    p = argparse.ArgumentParser(description="SMTP stress tester — orthogonal dimensions")
    p.add_argument("--mode", choices=list(MODE_DEFAULTS), default="mta-relay")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=0)  # 0 = use mode default
    p.add_argument("--user", "-u"); p.add_argument("--password", "-p")
    p.add_argument("--tls", choices=["none","starttls","implicit"])
    p.add_argument("--pipeline", action="store_true",
                   help="Batch all SMTP commands in one TCP write")
    p.add_argument("--reuse-conn", action="store_true",
                   help="Reuse TCP connection for many messages")
    p.add_argument("--messages", type=int, default=1000)
    p.add_argument("--concurrency", type=int, default=50)
    p.add_argument("--timeout", type=int, default=10)
    p.add_argument("--ramp", action="store_true")
    p.add_argument("--ramp-start", type=int, default=50)
    p.add_argument("--ramp-end", type=int, default=400)
    p.add_argument("--ramp-step", type=int, default=50)
    p.add_argument("--scan", action="store_true")
    p.add_argument("--verbose", "-v", action="store_true")
    args = p.parse_args()

    # Apply mode defaults
    preset = MODE_DEFAULTS[args.mode]
    if args.port == 0: args.port = preset["port"]
    if not args.tls: args.tls = preset["tls"]
    if preset["auth"] and not args.user:
        p.error(f"--mode {args.mode} requires --user and --password")

    if args.scan:       run_scan(args)
    elif args.ramp:     run_ramp(args)
    else:               run_benchmark(args, f"{args.mode} pipe={args.pipeline} reuse={args.reuse_conn}")


if __name__ == "__main__":
    main()
