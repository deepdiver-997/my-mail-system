#ifndef MOCK_IO_CONTEXT_H
#define MOCK_IO_CONTEXT_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace mail_system {
namespace test {

// ================================================================
// MockIoContext — 简化版 asio io_context
//
//   内部维护任务队列；异步读/写完成被包装成任务 post 到此，
//   由独立工作线程执行（还原 asio 的 "completion handler 投递到
//   executor，由线程运行" 语义，实现跨线程投递用户缓冲区内容）。
//
//   两种执行模式：
//     - 同步模式（默认，未 start）：post 立即内联执行。供串行 FSM
//       逻辑测试使用（process_event 同步完成，断言无需等待）。
//     - 线程模式（start 后）：任务入队，由工作线程按 FIFO 执行。
//       供并发/异步测试使用，模拟真实 io_context 线程。
//
//   一个 context 只服务一个连接（本 mock 的简化假设）。
// ================================================================
class MockIoContext {
public:
    MockIoContext() = default;
    ~MockIoContext() { stop(); }

    MockIoContext(const MockIoContext&) = delete;
    MockIoContext& operator=(const MockIoContext&) = delete;

    // 投递任务。同步模式直接执行；线程模式入队。
    void post(std::function<void()> f) {
        bool run_inline = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!threaded_) {
                run_inline = true;
            } else {
                inflight_.fetch_add(1, std::memory_order_relaxed);
                tasks_.push_back(std::move(f));
            }
        }
        if (run_inline) {
            f();
            return;
        }
        cv_.notify_one();
    }

    // 启动工作线程（进入线程模式）
    void start() {
        std::lock_guard<std::mutex> lk(mu_);
        if (threaded_) return;
        threaded_ = true;
        worker_ = std::thread([this] { run_loop(); });
    }

    // 停止工作线程（join）。队列中剩余任务会执行完毕。
    void stop() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            threaded_ = false;
        }
        cv_.notify_one();
        if (worker_.joinable()) worker_.join();
    }

    bool threaded() const { return threaded_.load(std::memory_order_acquire); }

    // 等待所有已投递任务执行完毕（含任务内链式投递的新任务）。
    // 同步模式下恒为立即返回。
    bool wait_idle(int timeout_ms = 3000) {
        std::unique_lock<std::mutex> lk(mu_);
        if (!threaded_) return true;
        return cv_idle_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
            [this] { return inflight_.load(std::memory_order_acquire) == 0; });
    }

private:
    void run_loop() {
        std::unique_lock<std::mutex> lk(mu_);
        while (threaded_ || !tasks_.empty()) {
            if (tasks_.empty()) {
                cv_.wait(lk, [this] { return !threaded_ || !tasks_.empty(); });
                continue;
            }
            auto f = std::move(tasks_.front());
            tasks_.pop_front();
            lk.unlock();
            f();
            inflight_.fetch_sub(1, std::memory_order_release);
            cv_idle_.notify_all();
            lk.lock();
        }
    }

    std::mutex mu_;
    std::condition_variable cv_;
    std::condition_variable cv_idle_;
    std::deque<std::function<void()>> tasks_;
    std::atomic<bool> threaded_{false};
    std::atomic<int> inflight_{0};
    std::thread worker_;
};

} // namespace test
} // namespace mail_system
#endif
