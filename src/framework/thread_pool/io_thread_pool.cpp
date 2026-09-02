#include "framework/thread_pool/io_thread_pool.h"
#include "framework/thread_pool/io_context_registry.h"
#include "mail_system/back/common/logger.h"

namespace mail_system {

IOThreadPool::IOThreadPool(size_t thread_count)
    : m_thread_count(thread_count), m_running(false) {
    // 每个 io 线程必须持有【各自独立的】io_context，形成"一连接一 context 一线程"的
    // 并发模型（连接轮转绑定其一，其读写完成只在那一线程调度）。
    //
    // 坑：std::vector<T>(count, value) 对 shared_ptr<T> 是把同一个 make_shared 结果
    // 复制 count 份——若直接写 m_io_contexts(thread_count, std::make_shared<io_context>())，
    // 会让所有线程 run 同一个 io_context，等于一个多线程共享队列：
    //   同一条连接的读回调与写完成回调由任意 io 线程并发调度 → 无保护地抢共享缓冲
    //   （H2 连续多帧时 read-drain 与 write-complete 交叉，ASan 报 out_pending_ UAF）。
    // 显式逐索引 make_shared，避免这个"复制同指针"陷阱。
    m_io_contexts.reserve(thread_count);
    for (size_t i = 0; i < thread_count; ++i)
        m_io_contexts.push_back(std::make_shared<boost::asio::io_context>());
}

IOThreadPool::~IOThreadPool() {
    stop(true);
}

void IOThreadPool::start() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_running) return;

    m_running = true;
    LOG_THREAD_POOL_INFO("Starting IOThreadPool...");

    m_work_guards.reserve(m_thread_count);
    for (size_t i = 0; i < m_thread_count; ++i)
        m_work_guards.emplace_back(boost::asio::make_work_guard(
            m_io_contexts[i]->get_executor()));

    m_threads.reserve(m_thread_count);
    for (size_t i = 0; i < m_thread_count; ++i) {
        m_threads.emplace_back([this, i]() {
            // 注册 thread_local io_context：DB 非阻塞等待（mariadb async）据此
            // 决定用 io_context.async_wait（io 线程不阻塞）还是阻塞 poll。
            set_current_io_context(m_io_contexts[i].get());
            try {
                m_io_contexts[i]->run();
            } catch (const std::exception& e) {
                LOG_THREAD_POOL_ERROR("Exception in IO thread: {}", e.what());
            } catch (...) {
                LOG_THREAD_POOL_ERROR("Unknown exception in IO thread");
            }
            set_current_io_context(nullptr);
        });
    }
}

void IOThreadPool::stop(bool wait_for_tasks) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_running) return;
        m_running = false;  // 阻止 get_io_context() 继续分发任务
    }
    LOG_THREAD_POOL_INFO("Stopping IOThreadPool...");

    // 释放 work guard → 各工作线程的 run() 在待处理任务完成后返回
    for (auto& wg : m_work_guards) wg.reset();

    if (!wait_for_tasks) {
        // 强制打断：stop() 使所有 run()/run_one() 立即返回
        for (auto& ctx : m_io_contexts) ctx->stop();
    }
    // wait_for_tasks=true: 工作线程自然排空队列后 run() 返回

    for (auto& t : m_threads) {
        if (t.joinable()) t.join();
    }
}

size_t IOThreadPool::thread_count() const {
    return m_thread_count;
}

bool IOThreadPool::is_running() const {
    return m_running.load();
}

}