#ifndef MAIL_SYSTEM_IO_THREAD_POOL_H
#define MAIL_SYSTEM_IO_THREAD_POOL_H

#include "thread_pool_base.h"
#include <boost/asio/io_context.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/strand.hpp>
#include <iostream>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>

namespace mail_system {
/**
 * @brief IO线程池实现
 * 
 * 这个类使用Boost.Asio的io_context来实现IO线程池功能，
 * 适用于处理IO密集型任务，如网络通信、文件操作等。
 */
class IOThreadPool : public ThreadPoolBase {
public:
    /**
     * @brief 构造函数
     * 
     * @param thread_count 线程数量，默认为系统硬件并发数
     */
    explicit IOThreadPool(size_t thread_count = std::thread::hardware_concurrency())
        : m_thread_count(thread_count), m_io_context(std::make_shared<boost::asio::io_context>()),
          m_running(false) {
    }

    /**
     * @brief 析构函数
     * 
     * 确保线程池在销毁前停止
     */
    ~IOThreadPool() override {
        stop(true);
    }

    /**
     * @brief 启动线程池
     */
    void start() override {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_running) return;

        m_running = true;
        m_work_guard = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(
            m_io_context->get_executor());

        m_threads.reserve(m_thread_count);
        for (size_t i = 0; i < m_thread_count; ++i) {
            m_threads.emplace_back([this]() {
                try {
                    m_io_context->run();
                } catch (const std::exception& e) {
                    std::cerr << "Exception in IO thread: " << e.what() << std::endl;
                } catch (...) {
                    std::cerr << "Unknown exception in IO thread" << std::endl;
                }
            });
        }
    }

    /**
     * @brief 停止线程池
     * 
     * @param wait_for_tasks 是否等待所有任务完成
     */
    void stop(bool wait_for_tasks = true) override {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (!m_running) return;

        m_running = false;

        // 移除工作守卫，允许io_context在没有更多工作时退出
        if (m_work_guard) {
            m_work_guard.reset();
        }

        // 如果不等待任务完成，则停止io_context
        if (!wait_for_tasks) {
            m_io_context->stop();
        }

        // 复制线程列表，以便在解锁后等待它们
        auto threads = std::move(m_threads);
        lock.unlock();

        // 等待所有线程完成
        for (auto& thread : threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }

        // 重置io_context以便重新使用
        lock.lock();
        m_io_context->restart();
    }

    /**
     * @brief 获取线程池中的线程数量
     * 
     * @return size_t 线程数量
     */
    size_t thread_count() const override {
        return m_thread_count;
    }

    /**
     * @brief 获取io_context
     * 
     * @return std::shared_ptr<boost::asio::io_context> io_context的共享指针
     */
    std::shared_ptr<boost::asio::io_context> get_io_context() const {
        return m_io_context;
    }

protected:
    /**
     * @brief 提交任务的实现
     * 
     * @tparam F 任务函数类型
     * @tparam Args 任务函数参数类型
     * @param f 任务函数
     * @param args 任务函数参数
     * @return std::future<typename std::result_of<F(Args...)>::type> 任务结果的future
     */
    template<class F, class... Args>
    auto submit_impl(F&& f, Args&&... args) 
        -> std::future<typename std::result_of<F(Args...)>::type> {
        using return_type = typename std::result_of<F(Args...)>::type;

        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_running) {
            throw std::runtime_error("Thread pool is not running");
        }

        auto task = std::make_shared<std::packaged_task<return_type()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );

        std::future<return_type> result = task->get_future();
        boost::asio::post(*m_io_context, [task]() { (*task)(); });
        return result;
    }

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
        boost::asio::post(*m_io_context, f);
    }

private:
    size_t m_thread_count;                          ///< 线程数量
    std::shared_ptr<boost::asio::io_context> m_io_context; ///< IO上下文
    std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> m_work_guard; ///< 工作守卫
    std::vector<std::thread> m_threads;             ///< 线程列表
    bool m_running;                                 ///< 线程池是否运行中
    std::mutex m_mutex;                             ///< 互斥锁，保护线程池状态
};

} // namespace mail_system

#endif // MAIL_SYSTEM_IO_THREAD_POOL_H