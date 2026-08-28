// POP3 读路径基准客户端 — raw sockets, no asio.
// 测量 POP3 读路径（STAT + LIST + RETR 1:N）的吞吐/延迟。
//
// 用法:
//   ./pop3_client                          # 4 线程 × 2 连接 × 100 轮, RETR 1:5
//   ./pop3_client --t 8 --conns 4 --rounds 500 --mails 10
//
// 每个线程开 --conns 条连接，每条连接循环 --rounds 轮（USER/PASS 一次后反复
// STAT+LIST+RETR），统计每轮延迟 P50/P95/P99 + 总吞吐。
// 每轮 = STAT + LIST + RETR 1:N。读场景不产生新数据，无需清理。

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

static int tcp_connect(const char* host, int port, int timeout_sec) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
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

// 读单行（+OK/-ERR 状态行）。
static bool read_line(FILE* f, std::string& out) {
    char buf[4096];
    if (!fgets(buf, sizeof(buf), f)) return false;
    out = buf;
    if (!out.empty() && out.back() == '\n') out.pop_back();
    if (!out.empty() && out.back() == '\r') out.pop_back();
    return true;
}

// 读到 "." 终止的多行响应（LIST/RETR）。返回是否以 +OK 开头。
static bool read_multiline(FILE* f) {
    std::string line;
    if (!read_line(f, line)) throw std::runtime_error("eof before +OK");
    if (line.rfind("+OK", 0) != 0) return false;
    while (read_line(f, line)) {
        if (line == ".") return true;   // 多行结束标记
    }
    throw std::runtime_error("eof in multiline");
}

static void pop3_login(FILE* f, int fd, const std::string& user, const std::string& pass) {
    std::string line;
    // greeting
    if (!read_line(f, line)) throw std::runtime_error("no greeting");
    std::string cmd = "USER " + user + "\r\n";
    write_all(fd, cmd.c_str(), cmd.size());
    if (!read_line(f, line) || line.rfind("+OK", 0) != 0) throw std::runtime_error("USER failed: " + line);
    cmd = "PASS " + pass + "\r\n";
    write_all(fd, cmd.c_str(), cmd.size());
    if (!read_line(f, line) || line.rfind("+OK", 0) != 0) throw std::runtime_error("PASS failed: " + line);
}

struct Cfg {
    const char* host = "127.0.0.1";
    int port = 110;
    int threads = 4;
    int conns = 2;
    int rounds = 100;
    int mails = 5;            // RETR 1:N
    std::vector<std::string> users;   // 每条连接轮询用不同用户（POP3 单会话锁：
                                      // 同用户并发连接会抢锁失败，必须多用户并发）
    const char* pass = "test123";
};

static std::vector<uint64_t> g_latencies;
static std::mutex g_mu;
static std::atomic<uint64_t> g_rounds{0};

static void worker(const Cfg& cfg, int conn_i) {
    for (int c = 0; c < cfg.conns; ++c) {
        int fd = tcp_connect(cfg.host, cfg.port, 5);
        if (fd < 0) { std::cerr << "connect fail\n"; continue; }
        FILE* f = fdopen(fd, "r");
        if (!f) { close(fd); continue; }
        setvbuf(f, nullptr, _IOLBF, 0);

        // 每连接轮询一个用户：POP3 每用户单会话锁，同用户并发必抢锁失败
        std::string user = cfg.users.empty() ? "test2@scut.email"
                            : cfg.users[(conn_i * cfg.conns + c) % cfg.users.size()];
        try {
            pop3_login(f, fd, user, cfg.pass);
            std::vector<uint64_t> local;
            for (int r = 0; r < cfg.rounds; ++r) {
                auto t0 = std::chrono::steady_clock::now();
                std::string line;
                write_all(fd, "STAT\r\n", 6);
                if (!read_line(f, line) || line.rfind("+OK", 0) != 0) throw std::runtime_error("STAT: " + line);
                write_all(fd, "LIST\r\n", 6);
                read_multiline(f);                       // +OK + n size 列表
                for (int i = 1; i <= cfg.mails; ++i) {
                    std::string cmd = "RETR " + std::to_string(i) + "\r\n";
                    write_all(fd, cmd.c_str(), cmd.size());
                    read_multiline(f);                   // +OK + 正文 + .
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
        else if (a == "--users") {   // 逗号分隔，多用户并发绕过单会话锁
            std::string u = next();
            size_t pos = 0;
            while ((pos = u.find(',')) != std::string::npos) {
                cfg.users.push_back(u.substr(0, pos));
                u.erase(0, pos + 1);
            }
            if (!u.empty()) cfg.users.push_back(u);
        }
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
        return g_latencies[idx] / 1000.0;
    };

    uint64_t rounds = g_rounds.load();
    std::cout << "=== POP3 read bench ===\n"
              << "  threads=" << cfg.threads << " conns/thread=" << cfg.conns
              << " rounds/conn=" << cfg.rounds
              << " retr=" << cfg.mails << "\n"
              << "  wall=" << wall_s << "s  rounds=" << rounds
              << "  throughput=" << (rounds / wall_s) << " rounds/s\n"
              << "  latency(ms) P50=" << pct(0.50)
              << "  P95=" << pct(0.95)
              << "  P99=" << pct(0.99) << "\n";
    return 0;
}
