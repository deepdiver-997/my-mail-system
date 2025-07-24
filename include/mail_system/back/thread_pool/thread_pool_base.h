#ifndef MAIL_SYSTEM_THREAD_POOL_BASE_H
#define MAIL_SYSTEM_THREAD_POOL_BASE_H

#include <functional>
#include <memory>
#include <future>
#include <atomic>
#include <mail_system/back/mailServer/session/session_base.h>

namespace mail_system {

/**
 * @brief 线程池基类
 * 
 * 这是一个纯虚类，定义了线程池的基本接口。
 * 子类需要实现这些接口以提供具体的线程池功能。
 */
class ThreadPoolBase {
public:
    /**
     * @brief 虚析构函数
     */
    virtual ~ThreadPoolBase() = default;

    /**
     * @brief 向线程池提交任务
     * 
     * @tparam F 任务函数类型
     * @tparam Args 任务函数参数类型
     * @param f 任务函数
     * @param args 任务函数参数
     * @return std::future<typename std::result_of<F(Args...)>::type> 任务结果的future
     */
    template<class F, class... Args>
    auto submit(F&& f, Args&&... args) 
        -> std::future<typename std::result_of<F(Args...)>::type> {
        return submit_impl(std::forward<F>(f), std::forward<Args>(args)...);
    }

    /**
     * @brief 向线程池提交任务（无返回值版本）
     * 
     * @tparam F 任务函数类型
     * @param f 任务函数
     */
    template<class F>
    void post(F&& f) {
        post_impl(std::forward<F>(f));
    }

    /**
     * @brief 启动线程池
     */
    virtual void start() = 0;

    /**
     * @brief 停止线程池
     * 
     * @param wait_for_tasks 是否等待所有任务完成
     */
    virtual void stop(bool wait_for_tasks = true) = 0;

    /**
     * @brief 获取线程池中的线程数量
     * 
     * @return size_t 线程数量
     */
    virtual size_t thread_count() const = 0;
    virtual bool is_running() const = 0;

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
        -> std::future<typename std::result_of<F(Args...)>::type>;

    /**
     * @brief 提交任务的实现（无返回值版本）
     * 
     * @tparam F 任务函数类型
     * @param f 任务函数
     */
    virtual void post_impl(std::function<void()> f) = 0;
};

} // namespace mail_system

#endif // MAIL_SYSTEM_THREAD_POOL_BASE_H