#ifndef MAIL_SYSTEM_BOOST_THREAD_POOL_H
#define MAIL_SYSTEM_BOOST_THREAD_POOL_H

#include "thread_pool_base.h"
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/bind.hpp>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>

namespace mail_system {

/**
 * @brief 基于Boost的线程池实现
 * 
 * 这个类使用Boost.Asio的thread_pool来实现线程池功能，
 * 适用于处理CPU密集型或阻塞性任务。
 */
class BoostThreadPool : public ThreadPoolBase {
public:
    /**
     * @brief 构造函数
     * 
     * @param thread_count 线程数量，默认为系统硬件并发数
     */
    explicit BoostThreadPool(size_t thread_count = std::thread::hardware_concurrency());

    /**
     * @brief 析构函数
     * 
     * 确保线程池在销毁前停止
     */
    ~BoostThreadPool();

    /**
     * @brief 启动线程池
     */
        void start() override; // Prepare to add override keyword

    /**
     * @brief 停止线程池
     * 
     * @param wait_for_tasks 是否等待所有任务完成
     */
        void stop(bool wait_for_tasks = true) override; // Prepare to add override keyword

    /**
     * @brief 获取线程池中的线程数量
     * 
     * @return size_t 线程数量
     */
        size_t thread_count() const override; // Prepare to add override keyword

    /**
     * @brief 检查线程池是否正在运行
     * 
     * @return true 如果线程池正在运行
     * @return false 如果线程池已停止
     */
        bool is_running() const override; // Prepare to add override keyword

protected:
    /**
     * @brief 提交任务的实现（无返回值版本）
     * 
     * @param f 任务函数
     */
    void post_impl(std::function<void()> f) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_running) {
            throw std::runtime_error("Thread pool is not running");
        }
        boost::asio::post(*m_pool, f);
    }

private:
    size_t m_thread_count;                          ///< 线程数量
    std::unique_ptr<boost::asio::thread_pool> m_pool; ///< Boost线程池
    std::atomic<bool> m_running;                                 ///< 线程池是否运行中
    std::mutex m_mutex;                             ///< 互斥锁，保护线程池状态
};

} // namespace mail_system

#endif // MAIL_SYSTEM_BOOST_THREAD_POOL_H