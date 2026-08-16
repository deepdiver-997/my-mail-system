#!/usr/bin/env python3
"""
ProtoRelay 邀请码注册服务
启动: uvicorn main:app --host 127.0.0.1 --port 8080
前面用 nginx/Caddy 反代并加 SSL，或直接用 SSH 本地转发访问。
"""

import json, re, time, os, subprocess
from datetime import datetime, timedelta
from typing import Optional

import mysql.connector
from fastapi import FastAPI, HTTPException, Request
from pydantic import BaseModel

# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------
BASE_DIR = os.path.dirname(os.path.abspath(__file__))
with open(os.path.join(BASE_DIR, "config.json")) as f:
    CONFIG = json.load(f)

DB_CONFIG = {
    "host": CONFIG["db_host"],
    "user": CONFIG["db_user"],
    "password": CONFIG["db_password"],
    "database": CONFIG["db_database"],
    "charset": "utf8mb4",
}

# C++ bcrypt tool (must match server's bcrypt implementation)
HASH_TOOL = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "hash_tool")

def bcrypt_hash(password: str) -> str:
    """使用 C++ hash_tool 生成与服务器完全兼容的 bcrypt hash"""
    result = subprocess.run([HASH_TOOL, password], capture_output=True, text=True, timeout=5)
    if result.returncode != 0:
        raise RuntimeError(f"hash_tool failed: {result.stderr}")
    return result.stdout.strip()

app = FastAPI(title="ProtoRelay Register", version="1.0")

# ---------------------------------------------------------------------------
# Rate limiter (in-memory, resets on restart — enough for personal use)
# ---------------------------------------------------------------------------
_rate_buckets: dict[str, list[float]] = {}

def check_rate_limit(ip: str) -> bool:
    now = time.time()
    window = CONFIG["rate_limit_window_hours"] * 3600
    max_req = CONFIG["rate_limit_per_ip"]
    bucket = _rate_buckets.get(ip, [])
    bucket = [t for t in bucket if now - t < window]
    if len(bucket) >= max_req:
        return False
    bucket.append(now)
    _rate_buckets[ip] = bucket
    return True

# ---------------------------------------------------------------------------
# DB helpers
# ---------------------------------------------------------------------------
def get_db():
    return mysql.connector.connect(**DB_CONFIG)

# ---------------------------------------------------------------------------
# Models
# ---------------------------------------------------------------------------
class RegisterRequest(BaseModel):
    invite_code: str = ""
    password: str = ""

class StatsResponse(BaseModel):
    invite_code: str
    used: int
    max_uses: int
    remaining: int

# ---------------------------------------------------------------------------
# Routes
# ---------------------------------------------------------------------------
@app.get("/")
def root():
    return {"service": "ProtoRelay Invite Registration", "version": "1.0"}

@app.get("/health")
def health():
    return {"status": "ok"}

@app.get("/stats/{code}")
def stats(code: str):
    """查询邀请码使用情况"""
    db = get_db()
    try:
        cur = db.cursor(dictionary=True)
        cur.execute(
            "SELECT code, max_uses, used_count, is_active "
            "FROM invite_codes WHERE code = %s", (code,))
        row = cur.fetchone()
        if not row:
            raise HTTPException(404, "Invite code not found")
        return StatsResponse(
            invite_code=row["code"],
            used=row["used_count"],
            max_uses=row["max_uses"],
            remaining=row["max_uses"] - row["used_count"],
        )
    finally:
        db.close()

@app.post("/register")
def register(req: RegisterRequest, request: Request):
    """邀请码注册: POST /register { invite_code, password }"""
    # IP rate limit
    ip = request.client.host if request.client else "unknown"
    if not check_rate_limit(ip):
        raise HTTPException(429, "Too many attempts, try again later")

    # Validation
    code = req.invite_code.strip()
    pwd = req.password
    if not code or not pwd:
        raise HTTPException(400, "invite_code and password required")
    if len(pwd) < 4:
        raise HTTPException(400, "Password must be at least 4 characters")

    # Max email length check (invitor_999999@<DOMAIN> ≈ 30 chars, safe)
    if len(code) > 64:
        raise HTTPException(400, "Invite code too long")

    db = get_db()
    try:
        cur = db.cursor(dictionary=True)

        # Lookup invite code
        cur.execute(
            "SELECT id, max_uses, used_count, expires_days, is_active "
            "FROM invite_codes WHERE code = %s FOR UPDATE", (code,))
        row = cur.fetchone()
        if not row:
            raise HTTPException(400, "Invalid invite code")
        if not row["is_active"]:
            raise HTTPException(400, "Invite code is no longer active")
        if row["used_count"] >= row["max_uses"]:
            raise HTTPException(400, "Invite code has been fully used")

        # Assign sequential email: invitor_N@<register_domain>（域名来自 config.json）
        seq = row["used_count"] + 1
        email = f"invitor_{seq}@{CONFIG.get('register_domain', 'example.com')}"

        # Check email uniqueness
        cur.execute("SELECT id FROM users WHERE mail_address = %s", (email,))
        if cur.fetchone():
            raise HTTPException(409, f"Email {email} already exists — this should not happen")

        # bcrypt hash via C++ tool (compatible with server)
        hashed = bcrypt_hash(pwd)

        # ------- Insert user + update counters atomically
        cur.execute(
            "INSERT INTO users (mail_address, password, name) VALUES (%s, %s, %s)",
            (email, hashed, f"Invitor #{seq}"))
        user_id = cur.lastrowid

        # Trigger auto-creates mailboxes; verify
        cur.execute("SELECT COUNT(*) AS cnt FROM mailboxes WHERE user_id = %s", (user_id,))
        if cur.fetchone()["cnt"] == 0:
            # Manual fallback
            for box_name in ["收件箱", "发件箱", "垃圾箱", "草稿箱", "已删除"]:
                cur.execute(
                    "INSERT INTO mailboxes (user_id, name) VALUES (%s, %s)",
                    (user_id, box_name))

        # Calculate expiry
        expires_at = datetime.now() + timedelta(days=row["expires_days"])

        # Insert registration record
        cur.execute(
            "INSERT INTO invite_registrations "
            "(user_id, code_id, invite_code, seq_num, email, expires_at) "
            "VALUES (%s, %s, %s, %s, %s, %s)",
            (user_id, row["id"], code, seq, email, expires_at))

        # Increment used_count
        cur.execute(
            "UPDATE invite_codes SET used_count = used_count + 1 WHERE id = %s",
            (row["id"],))

        db.commit()

        return {
            "success": True,
            "email": email,
            "expires_at": expires_at.isoformat(),
            "note": f"Account expires after {row['expires_days']} days",
        }
    except HTTPException:
        db.rollback()
        raise
    except Exception as e:
        db.rollback()
        raise HTTPException(500, f"Registration failed: {e}")
    finally:
        db.close()

# ---------------------------------------------------------------------------
# Bootstrap: ensure invite codes exist in DB
# ---------------------------------------------------------------------------
def bootstrap_invite_codes():
    db = get_db()
    try:
        cur = db.cursor()
        for ic in CONFIG.get("invite_codes", []):
            cur.execute(
                "INSERT IGNORE INTO invite_codes (code, max_uses, expires_days) "
                "VALUES (%s, %s, %s)",
                (ic["code"], ic["max_uses"], ic.get("expires_days", 90)))
        db.commit()
    finally:
        db.close()

bootstrap_invite_codes()
print("Invite codes bootstrapped:", [ic["code"] for ic in CONFIG.get("invite_codes", [])])

# ---------------------------------------------------------------------------
# Run
# ---------------------------------------------------------------------------
if __name__ == "__main__":
    import uvicorn
    uvicorn.run("main:app", host=CONFIG["listen_host"], port=CONFIG["listen_port"], reload=True)
