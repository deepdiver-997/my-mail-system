#include "framework/thread_pool/boost_thread_pool.h"
#include "mail_system/back/common/logger.h"

namespace mail_system {

BoostThreadPool::BoostThreadPool(size_t thread_count)
    : m_thread_count(thread_count), m_running(false) {
}

BoostThreadPool::~BoostThreadPool() {
    stop(true);
}

void BoostThreadPool::start() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_running) {
        m_running = true;
        LOG_THREAD_POOL_INFO("Starting BoostThreadPool...");
        m_pool = std::make_unique<boost::asio::thread_pool>(m_thread_count);
    }
}

void BoostThreadPool::stop(bool wait_for_tasks) {
    std::unique_ptr<boost::asio::thread_pool> pool;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_running) return;
        m_running = false;
        pool = std::move(m_pool);
    }

    if (pool) {
        if (wait_for_tasks) {
            pool->join();
        }
        pool->stop();
        LOG_THREAD_POOL_INFO("Stopped BoostThreadPool");
        pool->join();
    }
}

size_t BoostThreadPool::thread_count() const {
    return m_thread_count;
}

bool BoostThreadPool::is_running() const {
    return m_running;
}

}