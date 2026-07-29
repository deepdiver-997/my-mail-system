// High-performance SMTP client for benchmarking — raw sockets, no asio.
// Usage: smtp_client [--seq] [--pipe] [--reuse] [--t N] [--msgs M]
//   ./smtp_client                        # pipeline, per-conn, 4 threads, 50000 msgs
//   ./smtp_client --seq --reuse --t 4    # sequential, reuse, 4 threads
//   ./smtp_client --pipe --reuse --t 8   # max throughput: pipeline + reuse, 8 threads
//   ./smtp_client --seq                  # sequential, per-conn

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>

// ── helpers ────────────────────────────────────────────────────────────────
static int tcp_connect(const char* local_ip, const char* host, int port, int timeout_sec) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;

    // 绑定本地 loopback IP（解决 ephemeral port 耗尽：每个 src_ip 独立 16K 端口范围）
    if (local_ip && local_ip[0]) {
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        struct sockaddr_in local{};
        local.sin_family = AF_INET;
        inet_pton(AF_INET, local_ip, &local.sin_addr);
        if (bind(fd, (struct sockaddr*)&local, sizeof(local)) < 0) {
            close(fd); return -1;
        }
    } else {
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    }

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, host, &addr.sin_addr);
    // non-blocking connect with timeout
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int r = connect(fd, (struct sockaddr*)&addr, sizeof(addr));
    if (r < 0 && errno != EINPROGRESS) { close(fd); return -1; }
    if (r == 0) { fcntl(fd, F_SETFL, flags); return fd; }
    fd_set wfds; FD_ZERO(&wfds); FD_SET(fd, &wfds);
    struct timeval tv{timeout_sec, 0};
    r = select(fd + 1, nullptr, &wfds, nullptr, &tv);
    if (r <= 0) { close(fd); return -1; }
    int err = 0; socklen_t len = sizeof(err);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
    if (err) { close(fd); return -1; }
    fcntl(fd, F_SETFL, flags);
    // TCP_NODELAY for localhost performance
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return fd;
}

static void write_all(int fd, const char* data, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, data, len);
        if (n <= 0) throw std::runtime_error("write failed");
        data += n; len -= static_cast<size_t>(n);
    }
}

static std::string read_line(FILE* f) {
    char buf[4096];
    if (!fgets(buf, sizeof(buf), f)) throw std::runtime_error("read failed");
    return {buf};
}

static void skip_multiline(FILE* f) {
    char buf[4096];
    while (fgets(buf, sizeof(buf), f))
        if (strlen(buf) >= 4 && buf[3] == ' ') return;
}

// ── message builders ──────────────────────────────────────────────────────
struct BenchConfig {
    const char* host = "127.0.0.1";
    int port = 25;
    int timeout = 5;
    int messages = 50000;
    int threads = 4;
    bool pipeline = true;
    bool reuse = false;
    std::vector<std::string> local_ips = {"127.0.0.1"};
};

static std::string g_senders[10], g_recipients[10];

static std::string build_pipeline_msg(int idx, bool need_ehlo, bool do_quit) {
    auto& s = g_senders[idx % 10];
    auto& r = g_recipients[idx % 10];
    std::string m;
    if (need_ehlo) m += "EHLO t\r\n";
    m += "MAIL FROM:<" + s + ">\r\n";
    m += "RCPT TO:<" + r + ">\r\n";
    m += "DATA\r\nFrom: " + s + "\r\nTo: " + r + "\r\nSubject: b\r\n\r\nhi\r\n.\r\n";
    if (do_quit) m += "QUIT\r\n";
    return m;
}

static std::string build_mail_body(int idx) {
    auto& s = g_senders[idx % 10];
    auto& r = g_recipients[idx % 10];
    return "From: " + s + "\r\nTo: " + r + "\r\nSubject: b\r\n\r\nhi\r\n.\r\n";
}

// ── workers ───────────────────────────────────────────────────────────────
// Sequential (no pipelining): one command + one response per round-trip
static void worker_seq_perconn(int idx, const BenchConfig& cfg,
                                std::atomic<size_t>& ok, std::atomic<size_t>& fail) {
    const auto& lip = cfg.local_ips[idx % cfg.local_ips.size()];
    int n = cfg.messages / cfg.threads + (idx < cfg.messages % cfg.threads ? 1 : 0);
    for (int i = 0; i < n; ++i) {
        int fd = tcp_connect(lip.c_str(), cfg.host, cfg.port, cfg.timeout);
        if (fd < 0) { fail.fetch_add(1); continue; }
        FILE* f = fdopen(fd, "r");
        if (!f) { close(fd); fail.fetch_add(1); continue; }
        try {
            read_line(f);                                 // banner
            write_all(fd, "EHLO t\r\n", 8);
            skip_multiline(f);                            // EHLO
            auto& s = g_senders[idx % 10];
            auto& r = g_recipients[idx % 10];
            auto cmd = "MAIL FROM:<" + s + ">\r\n";
            write_all(fd, cmd.data(), cmd.size());
            read_line(f);                                 // 250
            cmd = "RCPT TO:<" + r + ">\r\n";
            write_all(fd, cmd.data(), cmd.size());
            read_line(f);                                 // 250
            write_all(fd, "DATA\r\n", 6);
            read_line(f);                                 // 354
            auto body = build_mail_body(i);
            write_all(fd, body.data(), body.size());
            read_line(f);                                 // 250
            write_all(fd, "QUIT\r\n", 6);
            read_line(f);                                 // 221
            ok.fetch_add(1);
        } catch (...) { fail.fetch_add(1); }
        fclose(f); close(fd);
    }
}

static void worker_seq_reuse(int idx, const BenchConfig& cfg,
                              std::atomic<size_t>& ok, std::atomic<size_t>& fail) {
    const auto& lip = cfg.local_ips[idx % cfg.local_ips.size()];
    int n = cfg.messages / cfg.threads + (idx < cfg.messages % cfg.threads ? 1 : 0);
    if (n == 0) return;
    int fd = tcp_connect(lip.c_str(), cfg.host, cfg.port, cfg.timeout);
    if (fd < 0) { fail.fetch_add(static_cast<size_t>(n)); return; }
    FILE* f = fdopen(fd, "r");
    if (!f) { close(fd); fail.fetch_add(static_cast<size_t>(n)); return; }
    try {
        read_line(f);                                     // banner
        write_all(fd, "EHLO t\r\n", 8);
        skip_multiline(f);                                // EHLO
        for (int i = 0; i < n; ++i) {
            auto& s = g_senders[idx % 10];
            auto& r = g_recipients[idx % 10];
            auto cmd = "MAIL FROM:<" + s + ">\r\n";
            write_all(fd, cmd.data(), cmd.size());
            read_line(f);                                 // 250
            cmd = "RCPT TO:<" + r + ">\r\n";
            write_all(fd, cmd.data(), cmd.size());
            read_line(f);                                 // 250
            write_all(fd, "DATA\r\n", 6);
            read_line(f);                                 // 354
            auto body = build_mail_body(i);
            write_all(fd, body.data(), body.size());
            read_line(f);                                 // 250
            ok.fetch_add(1);
        }
        write_all(fd, "QUIT\r\n", 6);
        read_line(f);                                     // 221
    } catch (...) { fail.fetch_add(static_cast<size_t>(n)); }
    fclose(f); close(fd);
}

// Pipeline workers: batch all commands in one TCP write
static void worker_pipe_perconn(int idx, const BenchConfig& cfg,
                                std::atomic<size_t>& ok, std::atomic<size_t>& fail) {
    const auto& lip = cfg.local_ips[idx % cfg.local_ips.size()];
    int n = cfg.messages / cfg.threads + (idx < cfg.messages % cfg.threads ? 1 : 0);
    for (int i = 0; i < n; ++i) {
        int fd = tcp_connect(lip.c_str(), cfg.host, cfg.port, cfg.timeout);
        if (fd < 0) { fail.fetch_add(1); continue; }
        FILE* f = fdopen(fd, "r");
        if (!f) { close(fd); fail.fetch_add(1); continue; }
        try {
            read_line(f); // banner
            auto msg = build_pipeline_msg(i, true, true);
            write_all(fd, msg.data(), msg.size());
            skip_multiline(f);     // EHLO
            for (int j = 0; j < 4; ++j) read_line(f);
            ok.fetch_add(1);
        } catch (...) { fail.fetch_add(1); }
        fclose(f); close(fd);
    }
}

static void worker_pipe_reuse(int idx, const BenchConfig& cfg,
                              std::atomic<size_t>& ok, std::atomic<size_t>& fail) {
    const auto& lip = cfg.local_ips[idx % cfg.local_ips.size()];
    int n = cfg.messages / cfg.threads + (idx < cfg.messages % cfg.threads ? 1 : 0);
    if (n == 0) return;
    int fd = tcp_connect(lip.c_str(), cfg.host, cfg.port, cfg.timeout);
    if (fd < 0) { fail.fetch_add(static_cast<size_t>(n)); return; }
    FILE* f = fdopen(fd, "r");
    if (!f) { close(fd); fail.fetch_add(static_cast<size_t>(n)); return; }
    try {
        read_line(f); // banner
        for (int i = 0; i < n; ++i) {
            auto msg = build_pipeline_msg(i, i == 0, i == n - 1);
            write_all(fd, msg.data(), msg.size());
            if (i == 0) skip_multiline(f);
            for (int j = 0; j < 4; ++j) read_line(f);
            ok.fetch_add(1);
        }
    } catch (...) { fail.fetch_add(static_cast<size_t>(n)); }
    fclose(f); close(fd);
}

// ── main ───────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    BenchConfig cfg;
    for (int i = 0; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--seq") cfg.pipeline = false;
        else if (a == "--pipe") cfg.pipeline = true;
        else if (a == "--reuse") cfg.reuse = true;
        else if (a == "--t" && i + 1 < argc) cfg.threads = std::stoi(argv[++i]);
        else if (a == "--msgs" && i + 1 < argc) cfg.messages = std::stoi(argv[++i]);
        else if (a == "--port" && i + 1 < argc) cfg.port = std::stoi(argv[++i]);
        else if (a == "--host" && i + 1 < argc) cfg.host = argv[++i];
        else if (a == "--local-ips" && i + 1 < argc) {
            cfg.local_ips.clear();
            std::string ips = argv[++i];
            size_t pos = 0;
            while (pos < ips.size()) {
                auto comma = ips.find(',', pos);
                auto ip = ips.substr(pos, comma - pos);
                if (!ip.empty()) cfg.local_ips.push_back(ip);
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
            if (cfg.local_ips.empty()) cfg.local_ips = {"127.0.0.1"};
        }
    }

    for (int i = 0; i < 10; ++i) {
        g_senders[i]    = "user" + std::to_string(i) + "@scut.email";
        g_recipients[i] = "dest"  + std::to_string(i) + "@scut.email";
    }

    // select worker: pipeline × reuse → 4 combos
    decltype(&worker_seq_perconn) worker;
    if (cfg.pipeline && cfg.reuse)       worker = worker_pipe_reuse;
    else if (cfg.pipeline && !cfg.reuse) worker = worker_pipe_perconn;
    else if (!cfg.pipeline && cfg.reuse) worker = worker_seq_reuse;
    else                                 worker = worker_seq_perconn;
    std::atomic<size_t> ok{0}, fail{0};
    std::vector<std::thread> threads;

    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < cfg.threads; ++i)
        threads.emplace_back(worker, i, std::cref(cfg), std::ref(ok), std::ref(fail));
    for (auto& t : threads) t.join();
    auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();

    size_t o = ok.load(), f = fail.load();
    std::cout << "pipe=" << (cfg.pipeline ? 'Y' : 'N')
              << " reuse=" << (cfg.reuse ? 'Y' : 'N')
              << " threads=" << cfg.threads
              << " total=" << (o + f) << " ok=" << o << " fail=" << f
              << " elapsed=" << elapsed << "s rate=" << static_cast<size_t>(o / elapsed)
              << " msg/s" << std::endl;
    return f > 0 ? 1 : 0;
}
