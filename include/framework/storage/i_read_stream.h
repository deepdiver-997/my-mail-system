#ifndef PR_FRAMEWORK_STORAGE_I_READ_STREAM_H
#define PR_FRAMEWORK_STORAGE_I_READ_STREAM_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace pr {


// 单个存储对象的只读句柄。
//
// 契约：调用方**不得修改** view() 返回的内容。正因为有这条约定，后端才可以
// 零拷贝地实现它 —— 本地文件后端直接 mmap 映射，`read()` 那次
// 「page cache → 用户缓冲区」的拷贝和随之而来的堆分配都省掉了。
// 需要一块可以就地改的缓冲时，改用 IStorageProvider::read_all()。
//
// 生命周期：view() 指向本对象持有的内存，**本对象一旦销毁即失效**。
// 不要写成 `auto v = provider->open_read(k, e)->view();` —— 临时的流对象
// 当场析构，v 立刻悬空。必须先把流存进具名变量。
class IReadStream {
public:
    virtual ~IReadStream() = default;

    virtual std::string_view view() const = 0;
    virtual std::uint64_t size() const = 0;
    bool empty() const { return size() == 0; }
};

// 默认实现：内容已经在堆上（远程后端下载下来的那一份）。
class BufferedReadStream : public IReadStream {
public:
    explicit BufferedReadStream(std::string data) : data_(std::move(data)) {}

    std::string_view view() const override { return data_; }
    std::uint64_t size() const override { return data_.size(); }

private:
    std::string data_;
};


} // namespace pr

#endif // PR_FRAMEWORK_STORAGE_I_READ_STREAM_H
