#ifndef MAIL_SYSTEM_METRICS_SERVER_H
#define MAIL_SYSTEM_METRICS_SERVER_H

#include <boost/asio.hpp>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <shared_mutex>
#include <string>
#include <vector>

namespace mail_system {

// 线程安全的指标存储 + HTTP 端点
// 组件通过 push 接口提交指标，/metrics 和 /status 端点直接渲染存储数据
//
// 使用方式:
//   ServerBase 持有 shared_ptr<MetricsServer>
//   各组件持有 weak_ptr<MetricsServer>，析构时自动失效
//   组件在事件发生时 push: if (auto m = metrics.lock()) m->inc("x", ...);
class MetricsServer {
public:
    using LabelMap = std::map<std::string, std::string>;

    using ReloadHandler    = std::function<std::string()>;
    using RefreshProvider   = std::function<void()>;  // render 前刷新慢变化指标

    MetricsServer(boost::asio::io_context& io_ctx,
                  uint16_t port, const std::string& bind_address,
                  ReloadHandler reload_handler = nullptr,
                  RefreshProvider refresh = nullptr)
        : io_ctx_(io_ctx), acceptor_(io_ctx_), port_(port)
        , bind_address_(bind_address), reload_handler_(std::move(reload_handler))
        , refresh_provider_(std::move(refresh)), running_(false) {}

    ~MetricsServer() { stop(); }

    MetricsServer(const MetricsServer&) = delete;
    MetricsServer& operator=(const MetricsServer&) = delete;

    // ── 指标注入接口 (线程安全) ──────────────────────────────────

    // 值瞬时覆盖上一次
    void set_gauge(const std::string& name, const LabelMap& labels, double val) {
        std::unique_lock lock(mutex_);
        gauges_[make_key(name, labels)] = {build_help(name), val};
    }

    // 单调递增
    void inc_counter(const std::string& name, const LabelMap& labels, uint64_t delta = 1) {
        std::unique_lock lock(mutex_);
        auto& c = counters_[make_key(name, labels)];
        if (c.help.empty()) c.help = build_help(name);
        c.value += delta;
    }

    // 记录一次观测值 (累计 sum + count，用于计算 rate/avg)
    void observe(const std::string& name, const LabelMap& labels, double value) {
        std::unique_lock lock(mutex_);
        std::string key = make_key(name, labels);
        auto& h = histograms_[key];
        if (h.help.empty()) h.help = build_help(name);
        h.sum   += value;
        h.count += 1;
    }

    // ── HTTP 服务 ───────────────────────────────────────────────

    void start() {
        if (running_.exchange(true)) return;
        boost::asio::ip::tcp::endpoint ep(
            boost::asio::ip::make_address(bind_address_), port_);
        acceptor_.open(ep.protocol());
        acceptor_.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
        acceptor_.bind(ep);
        acceptor_.listen();
        do_accept();
    }

    void stop() {
        if (!running_.exchange(false)) return;
        boost::system::error_code ec;
        acceptor_.close(ec);
    }

private:
    // ── 渲染 ────────────────────────────────────────────────────

    struct GaugeEntry    { std::string help; double value = 0; };
    struct CounterEntry  { std::string help; uint64_t value = 0; };
    struct HistogramEntry { std::string help; double sum = 0; uint64_t count = 0; };

    // key = "name{label=val,...}" — 保证唯一性
    static std::string make_key(const std::string& name, const LabelMap& labels) {
        if (labels.empty()) return name;
        std::string k = name + "{";
        bool first = true;
        for (auto& [lk, lv] : labels) {
            if (!first) k += ",";
            k += lk + "=\"" + lv + "\"";
            first = false;
        }
        k += "}";
        return k;
    }

    static std::string build_help(const std::string& name) {
        return "Metric " + name;
    }

    // 解析 key 为 name + labels
    static std::pair<std::string, std::string> split_key(const std::string& key) {
        auto pos = key.find('{');
        if (pos == std::string::npos) return {key, ""};
        return {key.substr(0, pos), key.substr(pos)};
    }

    std::string build_metrics() {
        if (refresh_provider_) refresh_provider_();
        std::shared_lock lock(mutex_);
        std::string out;
        out.reserve(4096);
        for (auto& [key, g] : gauges_) {
            auto [name, labels] = split_key(key);
            out += "# HELP " + name + " " + g.help + "\n";
            out += "# TYPE " + name + " gauge\n";
            out += name;
            if (!labels.empty()) out += labels;
            out += " " + std::to_string(static_cast<int64_t>(g.value)) + "\n";
        }
        for (auto& [key, c] : counters_) {
            auto [name, labels] = split_key(key);
            out += "# HELP " + name + " " + c.help + "\n";
            out += "# TYPE " + name + " counter\n";
            out += name;
            if (!labels.empty()) out += labels;
            out += " " + std::to_string(c.value) + "\n";
        }
        for (auto& [key, h] : histograms_) {
            auto [name, labels] = split_key(key);
            out += "# HELP " + name + "_sum " + h.help + "\n";
            out += "# TYPE " + name + "_sum gauge\n";
            out += name + "_sum";
            if (!labels.empty()) out += labels;
            out += " " + std::to_string(h.sum) + "\n";
            out += "# HELP " + name + "_count " + h.help + "\n";
            out += "# TYPE " + name + "_count gauge\n";
            out += name + "_count";
            if (!labels.empty()) out += labels;
            out += " " + std::to_string(h.count) + "\n";
        }
        return out;
    }

    std::string build_status() {
        std::shared_lock lock(mutex_);
        std::string out = "{\n";
        bool first = true;
        for (auto& [key, g] : gauges_) {
            if (!first) out += ",\n";
            auto [name, labels] = split_key(key);
            out += "  \"" + key + "\": " + std::to_string(g.value);
            first = false;
        }
        for (auto& [key, c] : counters_) {
            if (!first) out += ",\n";
            out += "  \"" + key + "\": " + std::to_string(c.value);
            first = false;
        }
        out += "\n}\n";
        return out;
    }

    // ── HTTP ────────────────────────────────────────────────────

    void do_accept() {
        auto socket = std::make_shared<boost::asio::ip::tcp::socket>(io_ctx_);
        acceptor_.async_accept(*socket, [this, socket](const boost::system::error_code& ec) {
            if (!ec) handle_request(socket);
            if (running_.load()) do_accept();
        });
    }

    void handle_request(std::shared_ptr<boost::asio::ip::tcp::socket> socket) {
        auto buf = std::make_shared<boost::asio::streambuf>();
        boost::asio::async_read_until(*socket, *buf, "\r\n\r\n",
            [this, socket, buf](const boost::system::error_code& ec, std::size_t) {
                if (ec) return;
                std::string request;
                std::istream is(buf.get());
                std::getline(is, request);

                std::string method, path;
                size_t s = request.find(' '), e = request.find(' ', s + 1);
                if (s != std::string::npos && e != std::string::npos) {
                    method = request.substr(0, s);
                    path   = request.substr(s + 1, e - s - 1);
                }

                std::string status, content_type, body;
                if (path == "/metrics" || path == "/") {
                    status = "200 OK";
                    content_type = "text/plain; version=0.0.4";
                    body = build_metrics();
                } else if (path == "/status") {
                    status = "200 OK";
                    content_type = "application/json";
                    body = build_status();
                } else if (method == "POST" && path == "/reload" && reload_handler_) {
                    std::string result = reload_handler_();
                    status = (result.empty() || result == "OK") ? "200 OK" : "500 Internal Server Error";
                    content_type = "text/plain";
                    body = status == "200 OK" ? "OK" : result;
                } else if (path == "/health" || path == "/health/live") {
                    status = "200 OK"; content_type = "text/plain"; body = "OK";
                } else if (path == "/health/ready") {
                    body = build_metrics();
                    status = body.empty() ? "503 Service Unavailable" : "200 OK";
                    content_type = "text/plain";
                } else {
                    status = "404 Not Found"; content_type = "text/plain"; body = "Not Found";
                }

                auto resp = std::make_shared<std::string>();
                resp->reserve(256 + body.size());
                resp->append("HTTP/1.1 ").append(status).append("\r\n");
                resp->append("Content-Type: ").append(content_type).append("\r\n");
                resp->append("Content-Length: ").append(std::to_string(body.size())).append("\r\n");
                resp->append("Connection: close\r\n\r\n");
                resp->append(body);

                boost::asio::async_write(*socket, boost::asio::buffer(*resp),
                    [socket, resp](const boost::system::error_code&, std::size_t) {
                        boost::system::error_code ignored;
                        socket->shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
                    });
            });
    }

    boost::asio::io_context& io_ctx_;
    boost::asio::ip::tcp::acceptor acceptor_;
    uint16_t port_;
    std::string bind_address_;
    ReloadHandler reload_handler_;
    RefreshProvider refresh_provider_;
    std::atomic<bool> running_;

    mutable std::shared_mutex mutex_;
    std::map<std::string, GaugeEntry>    gauges_;
    std::map<std::string, CounterEntry>  counters_;
    std::map<std::string, HistogramEntry> histograms_;
};

} // namespace mail_system
#endif
