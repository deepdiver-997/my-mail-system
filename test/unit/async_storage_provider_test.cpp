// AsyncStorageProvider 装饰器回归测试。
//
// 装饰器的职责只有一件事：把内层 provider 的 ms 级阻塞网络操作从 io 线程
// 挪到 executor 线程。核心断言：
//   1. async_* 不在发起线程执行网络操作，而是经 executor 投递；
//   2. 同步方法全部透传，值与内层一致；
//   3. open_write 包装出的流：write_at 透传，commit_async 投递；
//   4. executor 为空时退化为内联。
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "mail_system/back/storage/async_storage_provider.h"

using namespace mail_system::storage;

static int g_pass = 0, g_fail = 0;

static void expect_true(bool c, const char* what) {
    if (c) { g_pass++; }
    else { g_fail++; std::printf("  FAIL %s\n", what); }
}
static void expect_num(std::uint64_t got, std::uint64_t want, const char* what) {
    if (got == want) { g_pass++; }
    else { g_fail++; std::printf("  FAIL %s: got %llu want %llu\n", what,
        (unsigned long long)got, (unsigned long long)want); }
}
static void expect_str(const std::string& got, const std::string& want, const char* what) {
    if (got == want) { g_pass++; }
    else { g_fail++; std::printf("  FAIL %s: got %zu bytes, want %zu bytes\n",
                                 what, got.size(), want.size()); }
}

// ========== 记录型假后端 ==========

class RecordingProvider : public IStorageProvider {
public:
    bool ensure_ready(IoError&) override { return true; }
    std::string build_mail_body_key(std::uint64_t id) override { return "k/" + std::to_string(id); }
    std::string build_attachment_key(std::uint64_t id, const std::string& n) override {
        return "a/" + std::to_string(id) + "/" + n;
    }
    bool append_binary(const std::string& key, const char* d, std::size_t n, IoError&) override {
        object = std::string(d, n); appended_keys.push_back(key);
        return true;
    }
    bool remove_object(const std::string&, IoError&) override { removed = true; return true; }
    bool read_all(const std::string& key, std::string& out, IoError& error) override {
        if (key != "k/1") { error = IoError::permanent("no such object: " + key); return false; }
        out = object;
        return true;
    }
    bool object_size(const std::string& key, std::uint64_t& size, IoError& error) override {
        if (key != "k/1") { error = IoError::permanent("no such object: " + key); return false; }
        size = object.size();
        return true;
    }

    std::string object;
    std::vector<std::string> appended_keys;
    bool removed = false;
};

// 手动 executor：把任务攒下来，测试决定何时、在哪个“线程身份”下执行
struct ManualExecutor {
    std::vector<std::function<void()>> tasks;

    AsyncStorageProvider::Executor fn() {
        return [this](std::function<void()> t) { tasks.push_back(std::move(t)); };
    }
};

// ========== 用例 ==========

// 1. async_read_all：网络操作经 executor，不在发起线程执行
static void test_async_read_all_dispatched_impl() {
    auto inner = std::make_shared<RecordingProvider>();
    inner->object = "mail body bytes";

    ManualExecutor exec;
    AsyncStorageProvider provider(inner, exec.fn());

    bool called = false;
    std::string got_data;
    provider.async_read_all("k/1", [&](bool ok, std::string data, const IoError& e) {
        called = true;
        if (ok) got_data = std::move(data);
        else got_data = "ERR:" + e.message;
    });
    expect_true(!called, "callback NOT fired before executor runs the task");
    expect_num(exec.tasks.size(), 1, "exactly one task dispatched");

    exec.tasks[0]();
    expect_true(called, "callback fires when task runs");
    expect_str(got_data, "mail body bytes", "read_all value matches inner");

    // 失败路径同样只经 executor
    ManualExecutor exec2;
    AsyncStorageProvider p2(inner, exec2.fn());
    bool failed = false;
    std::string got_err;
    p2.async_read_all("k/404", [&](bool ok, std::string, const IoError& e) {
        failed = !ok;
        got_err = e.message;
    });
    exec2.tasks[0]();
    expect_true(failed, "missing key reports failure");
    expect_true(!got_err.empty(), "failure fills error");
}

// 2. async_object_size / async_open_read 同理
static void test_async_size_and_open_read() {
    auto inner = std::make_shared<RecordingProvider>();
    inner->object = "0123456789";

    ManualExecutor exec;
    AsyncStorageProvider provider(inner, exec.fn());

    bool size_called = false;
    std::uint64_t got_size = 0;
    provider.async_object_size("k/1", [&](bool ok, std::uint64_t sz, const IoError&) {
        size_called = ok;
        got_size = sz;
    });
    expect_num(exec.tasks.size(), 1, "object_size dispatched");
    exec.tasks[0]();
    expect_true(size_called && got_size == 10, "object_size value matches");

    bool open_called = false;
    std::string view;
    provider.async_open_read("k/1", [&](std::unique_ptr<IReadStream> s, const IoError&) {
        open_called = static_cast<bool>(s);
        if (s) view = std::string(s->view());
    });
    expect_num(exec.tasks.size(), 2, "open_read dispatched");
    exec.tasks[1]();
    expect_true(open_called, "open_read returns stream");
    expect_str(view, "0123456789", "open_read view matches");
}

// 3. open_write 的包装流：write_at 透传、commit_async 投递、abort 透传
static void test_wrapped_write_stream() {
    auto inner = std::make_shared<RecordingProvider>();

    ManualExecutor exec;
    AsyncStorageProvider provider(inner, exec.fn());

    IoError err;
    auto stream = provider.open_write("k/1", err);
    expect_true(stream != nullptr, "open_write returns wrapped stream");

    expect_true(stream->write_at(0, "abc", 3, err), "write_at passes through");
    expect_num(exec.tasks.size(), 0, "write_at does not dispatch");
    expect_str(inner->appended_keys.empty() ? "" : inner->appended_keys[0], "k/1",
               "write reached inner (via default AppendWriteStream)");

    bool committed = false;
    stream->commit_async([&](bool ok, const IoError&) { committed = ok; });
    expect_true(!committed, "commit not done inline");
    expect_num(exec.tasks.size(), 1, "commit dispatched to executor");
    exec.tasks[0]();
    expect_true(committed, "commit runs on executor");

    stream->abort();
    expect_true(inner->removed, "abort passes through to inner");
}

// 4. 空 executor 退化为内联
static void test_null_executor_inline() {
    auto inner = std::make_shared<RecordingProvider>();
    inner->object = "inline data";

    AsyncStorageProvider provider(inner, nullptr);
    bool called = false;
    std::string got;
    provider.async_read_all("k/1", [&](bool ok, std::string data, const IoError&) {
        called = ok;
        got = std::move(data);
    });
    expect_true(called, "null executor: inline execution");
    expect_str(got, "inline data", "inline value matches");
}

int main() {
    std::printf("async_storage_provider_test\n");
    test_async_read_all_dispatched_impl();
    test_async_size_and_open_read();
    test_wrapped_write_stream();
    test_null_executor_inline();

    std::printf("  pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
