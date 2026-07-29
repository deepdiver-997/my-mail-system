#!/bin/bash
# ProtoRelay 全端口测试脚本
# 用法: ./test/test_ports.sh
# 前提: smtpsServer 和 imapsServer 已在本地运行

HOST="127.0.0.1"
USER="test2@scut.email"
PASS="test123"

echo "=============================================="
echo " SMTP 端口测试"
echo "=============================================="

# ---- Port 25: TCP + STARTTLS, auth_policy=off ----
echo ""
echo "--- SMTP :25 (TCP, STARTTLS, no AUTH) ---"
(echo "EHLO test"; sleep 0.5; echo "QUIT") | \
  openssl s_client -starttls smtp -connect $HOST:25 -quiet 2>/dev/null
echo ""

# ---- Port 465: SSL implicit, auth_policy=on ----
echo ""
echo "--- SMTP :465 (SSL implicit, AUTH on) ---"
(echo "EHLO test"; sleep 0.5; echo "QUIT") | \
  openssl s_client -connect $HOST:465 -quiet 2>/dev/null
echo ""

# ---- Port 587: TCP + STARTTLS, auth_policy=on ----
echo ""
echo "--- SMTP :587 (TCP, STARTTLS, AUTH on) ---"
(echo "EHLO test"; sleep 0.5; echo "QUIT") | \
  openssl s_client -starttls smtp -connect $HOST:587 -quiet 2>/dev/null
echo ""

echo "=============================================="
echo " IMAP 端口测试"
echo "=============================================="

# ---- Port 143: TCP + STARTTLS ----
echo ""
echo "--- IMAP :143 (TCP, STARTTLS) ---"
(echo "a1 CAPABILITY"; sleep 0.5; echo "a2 LOGOUT") | \
  openssl s_client -starttls imap -connect $HOST:143 -quiet 2>/dev/null
echo ""

# ---- Port 993: SSL implicit ----
echo ""
echo "--- IMAP :993 (SSL implicit) ---"
(echo "a1 CAPABILITY"; sleep 0.5; echo "a2 LOGOUT") | \
  openssl s_client -connect $HOST:993 -quiet 2>/dev/null
echo ""

echo "=============================================="
echo " 带认证的完整测试"
echo "=============================================="

# ---- SMTP :587 AUTH PLAIN ----
echo ""
echo "--- SMTP :587 AUTH PLAIN (test2@scut.email) ---"
python3 << PYEOF
import socket, ssl, base64, time

def smtp_auth(port, use_ssl, use_starttls):
    sock = socket.socket(); sock.settimeout(5); sock.connect(('$HOST', port))
    if use_ssl:
        ctx = ssl.create_default_context(); ctx.check_hostname = False; ctx.verify_mode = ssl.CERT_NONE
        sock = ctx.wrap_socket(sock)

    def recv(): return sock.recv(4096).decode()

    if not use_ssl:
        print("  ", recv().strip())  # greeting
        sock.send(b"EHLO test\r\n"); time.sleep(0.3); recv()

    if use_starttls:
        sock.send(b"STARTTLS\r\n"); time.sleep(0.3); print("  STARTTLS:", recv().strip())
        ctx = ssl.create_default_context(); ctx.check_hostname = False; ctx.verify_mode = ssl.CERT_NONE
        sock = ctx.wrap_socket(sock)

    sock.send(b"EHLO test\r\n"); time.sleep(0.3)
    ehlo = recv(); print("  AUTH advertised:", "AUTH" in ehlo)

    if "AUTH" in ehlo:
        auth = base64.b64encode(b"\0$USER\0$PASS").decode()
        sock.send(f"AUTH PLAIN {auth}\r\n".encode()); time.sleep(0.3)
        print("  AUTH PLAIN:", recv().strip())

    sock.close()

print("  NOTE: auth_policy=on on this port")
smtp_auth(587, use_ssl=False, use_starttls=True)
PYEOF

# ---- IMAP :143 LOGIN ----
echo ""
echo "--- IMAP :143 LOGIN (test2@scut.email) ---"
python3 << PYEOF
import socket, ssl, time

sock = socket.socket(); sock.settimeout(5); sock.connect(('$HOST', 143))
def recv(): return sock.recv(4096).decode()

print("  ", recv().strip())  # greeting
sock.send(b"a1 CAPABILITY\r\n"); time.sleep(0.3); recv()
sock.send(b"a2 STARTTLS\r\n"); time.sleep(0.3); print("  STARTTLS:", recv().strip())

ctx = ssl.create_default_context(); ctx.check_hostname = False; ctx.verify_mode = ssl.CERT_NONE
sock = ctx.wrap_socket(sock)

sock.send(b"a3 LOGIN $USER $PASS\r\n"); time.sleep(0.3)
print("  LOGIN:", recv().strip())
sock.send(b"a4 LOGOUT\r\n"); time.sleep(0.3)
sock.close()
PYEOF

echo ""
echo "  Done."
