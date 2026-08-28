// IMAP read-path benchmark client — raw sockets, no asio.
// 测量 IMAP 读路径（SELECT + FETCH）在 io 线程同步死等 DB 下的吞吐/延迟，
// 作为 Phase 2（DB 真异步）的 before 基线。
//
// 用法:
//   ./imap_client                       # 4 线程 × 2 连接 × 100 轮, FETCH 1:200
//   ./imap_client --t 8 --conns 4 --rounds 500 --mails 200
//   ./imap_client --select-only         # 只 SELECT（纯 DB stats 查询）
//
// 每个线程开 --conns 条连接，每条连接循环 --rounds 轮（LOGIN 一次后反复
// SELECT+FETCH），统计每轮延迟 P50/P95/P99 + 总吞吐。
// FETCH 用 (FLAGS RFC822.SIZE) —— 无 literal 响应，客户端按行读到 tagged OK。

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

static int tcp_connect(const char* local_ip, const char* host, int port, int timeout_sec) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    if (local_ip && local_ip[0]) {
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        struct sockaddr_in local{};
        local.sin_family = AF_INET;
        inet_pton(AF_INET, local_ip, &local.sin_addr);
        if (bind(fd, (struct sockaddr*)&local, sizeof(local)) < 0) { close(fd); return -1; }
    } else {
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    }
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, host, &addr.sin_addr);
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

// 读到 tagged 行（"<tag> OK/NO/BAD ..."）为止；untagged（"* ..."）全部跳过。
// 本压测的 FETCH 不用 literal，响应是完整行，fgets 安全。
static void read_until_tagged(FILE* f, const std::string& tag) {
    char buf[16384];
    while (fgets(buf, sizeof(buf), f)) {
        if (strncmp(buf, tag.c_str(), tag.size()) == 0) return;
    }
    throw std::runtime_error("eof before tagged " + tag);
}

static void imap_login(FILE* f, int fd, const std::string& user, const std::string& pass) {
    std::string cmd = "L1 LOGIN " + user + " " + pass + "\r\n";
    write_all(fd, cmd.c_str(), cmd.size());
    read_until_tagged(f, "L1");
}

struct Cfg {
    const char* host = "127.0.0.1";
    const char* local_ip = "";  // 绑本地 loopback 可规避 ephemeral port 耗尽；macOS 默认空
    int port = 1143;
    int threads = 4;
    int conns = 2;
    int rounds = 100;
    int mails = 200;          // FETCH 1:N
    bool select_only = false;
    const char* user = "test2@scut.email";
    const char* pass = "test123";
};

static std::vector<uint64_t> g_latencies;  // 收集全部轮延迟（us）
static std::mutex g_mu;
static std::atomic<uint64_t> g_rounds{0};

static void worker(const Cfg& cfg, int conn_i) {
    for (int c = 0; c < cfg.conns; ++c) {
        int fd = tcp_connect(cfg.local_ip, cfg.host, cfg.port, 5);
        if (fd < 0) { std::cerr << "connect fail\n"; continue; }
        FILE* f = fdopen(fd, "r");
        if (!f) { close(fd); continue; }
        setvbuf(f, nullptr, _IOLBF, 0);

        // greeting
        char buf[4096];
        if (!fgets(buf, sizeof(buf), f)) { fclose(f); continue; }
        try {
            imap_login(f, fd, cfg.user, cfg.pass);
            std::vector<uint64_t> local;
            for (int r = 0; r < cfg.rounds; ++r) {
                auto t0 = std::chrono::steady_clock::now();
                std::string tag = "T" + std::to_string(conn_i * 1000 + c * 100 + r);
                std::string cmd = tag + " SELECT INBOX\r\n";
                write_all(fd, cmd.c_str(), cmd.size());
                read_until_tagged(f, tag);
                if (!cfg.select_only) {
                    std::string tag2 = tag + "F";
                    std::string cmd2 = tag2 + " FETCH 1:" + std::to_string(cfg.mails) + " (FLAGS RFC822.SIZE)\r\n";
                    write_all(fd, cmd2.c_str(), cmd2.size());
                    read_until_tagged(f, tag2);
                }
                auto t1 = std::chrono::steady_clock::now();
                local.push_back(static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()));
                g_rounds.fetch_add(1, std::memory_order_relaxed);
            }
            {
                std::lock_guard<std::mutex> lk(g_mu);
                g_latencies.insert(g_latencies.end(), local.begin(), local.end());
            }
        } catch (const std::exception& e) {
            std::cerr << "worker error: " << e.what() << "\n";
        }
        fclose(f);
    }
}

int main(int argc, char* argv[]) {
    Cfg cfg;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() { return argv[++i]; };
        if (a == "--host") cfg.host = next();
        else if (a == "--port") cfg.port = std::stoi(next());
        else if (a == "--t") cfg.threads = std::stoi(next());
        else if (a == "--conns") cfg.conns = std::stoi(next());
        else if (a == "--rounds") cfg.rounds = std::stoi(next());
        else if (a == "--mails") cfg.mails = std::stoi(next());
        else if (a == "--select-only") cfg.select_only = true;
        else if (a == "--user") cfg.user = next();
        else if (a == "--pass") cfg.pass = next();
        else { std::cerr << "unknown arg " << a << "\n"; return 1; }
    }

    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> ts;
    for (int i = 0; i < cfg.threads; ++i)
        ts.emplace_back(worker, std::cref(cfg), i);
    for (auto& t : ts) t.join();
    auto t1 = std::chrono::steady_clock::now();
    double wall_s = std::chrono::duration<double>(t1 - t0).count();

    std::sort(g_latencies.begin(), g_latencies.end());
    auto pct = [&](double p) -> double {
        if (g_latencies.empty()) return 0;
        size_t idx = std::min(g_latencies.size() - 1, (size_t)(g_latencies.size() * p));
        return g_latencies[idx] / 1000.0;  // us -> ms
    };

    uint64_t rounds = g_rounds.load();
    std::cout << "=== IMAP read bench ===\n"
              << "  threads=" << cfg.threads << " conns/thread=" << cfg.conns
              << " rounds/conn=" << cfg.rounds
              << " fetch=" << (cfg.select_only ? "none" : ("1:" + std::to_string(cfg.mails))) << "\n"
              << "  wall=" << wall_s << "s  rounds=" << rounds
              << "  throughput=" << (rounds / wall_s) << " rounds/s\n"
              << "  latency(ms) P50=" << pct(0.50)
              << "  P95=" << pct(0.95)
              << "  P99=" << pct(0.99) << "\n";
    return 0;
}
