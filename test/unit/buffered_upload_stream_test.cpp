// 整对象缓冲上传流回归测试。
//
// 远程后端（S3/WebHDFS）的流式写入改为「内存缓冲整封 + commit 一次上传」，
// 核心不变量：
//   1. write_at 只攒缓冲，upload 在 commit 前绝不被调用；
//   2. commit 恰好调用一次 upload，内容等于全部写入按 offset 拼接；
//   3. 乱序 offset 与超出容量都必须显式失败，而不是静默写坏/吃光内存；
//   4. commit 从未尝试过就 abort，远端零副作用（不 upload 也不 cleanup）；
//      commit 尝试失败后 abort，必须 cleanup 清掉远端可能的半成品。
#undef NDEBUG
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "framework/storage/buffered_upload_stream.h"
#include "mail_system/back/storage/mail_body_writer.h"

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

// ========== 记录型假上传后端 ==========

struct UploadLog {
    std::vector<std::string> uploads;   // 每次 upload 收到的完整对象
    bool fail_next_upload = false;
    int  cleanups = 0;                  // cleanup（远端清除）调用次数
};

static BufferedUploadStream::UploadFn recording_upload(UploadLog* log) {
    return [log](const char* data, std::size_t size, std::string& error) {
        if (log->fail_next_upload) {
            error = "injected upload failure";
            return false;
        }
        log->uploads.emplace_back(data, size);
        return true;
    };
}

static BufferedUploadStream::CleanupFn recording_cleanup(UploadLog* log) {
    return [log]() { log->cleanups++; };
}

static std::string make_payload(std::size_t n, unsigned seed) {
    std::string s;
    s.reserve(n);
    unsigned x = seed;
    for (std::size_t i = 0; i < n; ++i) {
        x = x * 1103515245u + 12345u;
        s.push_back(static_cast<char>('A' + (x >> 16) % 26));
    }
    return s;
}

// ========== 用例 ==========

// 1. 多块写入只攒缓冲，commit 一次上传完整对象
static void test_single_upload_at_commit() {
    UploadLog log;
    const std::string payload = make_payload(70000, 5);   // 故意超过 MailBodyWriter 默认 64KB
    {
        std::string err;
        MailBodyWriter w(std::make_unique<BufferedUploadStream>(
                             recording_upload(&log), recording_cleanup(&log), 1 << 20),
                         4096);   // 驱动 MailBodyWriter 多次 write_at
        std::size_t pos = 0;
        const std::size_t chunks[] = {1500, 8192, 1, 30000, 700};
        std::size_t ci = 0;
        while (pos < payload.size()) {
            std::size_t n = chunks[ci++ % (sizeof(chunks) / sizeof(chunks[0]))];
            n = std::min(n, payload.size() - pos);
            expect_true(w.write(payload.data() + pos, n, err), "chunk write ok");
            pos += n;
        }
        expect_num(log.uploads.size(), 0, "no upload before commit");
        expect_true(w.commit(err), "commit ok");
    }
    expect_num(log.uploads.size(), 1, "exactly one upload at commit");
    if (!log.uploads.empty()) {
        expect_str(log.uploads[0], payload, "uploaded object == full payload");
    }
    expect_num(log.cleanups, 0, "successful commit never cleans up");
}

// 2. 乱序 offset 必须被拒绝（与 AppendWriteStream 同一防错位契约）
static void test_out_of_order_rejected() {
    UploadLog log;
    std::string err;
    BufferedUploadStream s(recording_upload(&log), recording_cleanup(&log), 1024);
    expect_true(s.write_at(0, "abc", 3, err), "in-order write ok");
    expect_true(!s.write_at(8192, "tail", 4, err), "out-of-order write rejected");
    expect_true(err.find("out-of-order") != std::string::npos, "error names the cause");
    expect_num(log.uploads.size(), 0, "nothing uploaded");
}

// 3. 容量上限：超限失败且 upload 从未被调用；MailBodyWriter::failed() 随之置位
static void test_capacity_limit() {
    UploadLog log;
    std::string err;
    {
        MailBodyWriter w(std::make_unique<BufferedUploadStream>(
                             recording_upload(&log), recording_cleanup(&log), 100),
                         32);
        expect_true(w.write(make_payload(90, 1).data(), 90, err), "write under limit ok");
        expect_true(!w.failed(), "writer not failed yet");
        // 第二块必须大于 MailBodyWriter 自己的 32 字节缓冲，才会直穿到流上触发上限
        expect_true(!w.write(make_payload(40, 2).data(), 40, err), "write over limit fails");
        expect_true(!err.empty(), "over-limit fills error");
        expect_true(w.failed(), "writer marked failed");
        expect_true(!w.commit(err), "commit fails after capacity exceeded");
    }
    expect_num(log.uploads.size(), 0, "over-limit object never uploaded");
}

// 4. 未 commit 就析构：远端零副作用（不 upload、不 cleanup）
static void test_abort_without_upload_attempt() {
    UploadLog log;
    {
        std::string err;
        MailBodyWriter w(std::make_unique<BufferedUploadStream>(
                             recording_upload(&log), recording_cleanup(&log), 4096),
                         64);
        w.write("half a message", 14, err);
    }
    expect_num(log.uploads.size(), 0, "aborted stream never uploads");
    expect_num(log.cleanups, 0, "aborted before commit attempt: no remote cleanup needed");
}

// 5. commit 尝试失败 → abort 必须清远端半成品；此后 commit 恒败
static void test_failed_commit_cleans_up() {
    UploadLog log;
    log.fail_next_upload = true;
    std::string err;
    BufferedUploadStream s(recording_upload(&log), recording_cleanup(&log), 4096);
    expect_true(s.write_at(0, "body", 4, err), "write ok");
    expect_true(!s.commit(err), "commit fails");
    expect_true(!err.empty(), "commit failure fills error");
    s.abort();
    expect_num(log.cleanups, 1, "cleanup after failed commit attempt");

    std::string err2;
    expect_true(!s.commit(err2), "commit stays failed");
    expect_true(!s.write_at(4, "x", 1, err2), "write after failed commit rejected");
}

// 6. commit 成功后幂等：重复 commit 返回 true，析构不再 cleanup
static void test_commit_idempotent() {
    UploadLog log;
    {
        std::string err;
        BufferedUploadStream s(recording_upload(&log), recording_cleanup(&log), 4096);
        s.write_at(0, "mail", 4, err);
        expect_true(s.commit(err), "first commit ok");
        expect_true(s.commit(err), "second commit still true");
        s.abort();   // commit 之后的 abort 必须是 no-op
    }
    expect_num(log.uploads.size(), 1, "uploaded exactly once");
    expect_num(log.cleanups, 0, "no cleanup after successful commit");
}

// 7. 空对象：commit 仍要以 size==0 调一次 upload
//    （本地后端 open 即创建空文件，远程必须对齐，否则读侧读不到空邮件）
static void test_empty_object_still_uploads() {
    UploadLog log;
    std::string err;
    BufferedUploadStream s(recording_upload(&log), recording_cleanup(&log), 4096);
    expect_true(s.commit(err), "empty commit ok");
    expect_num(log.uploads.size(), 1, "empty object uploaded once");
    if (!log.uploads.empty()) {
        expect_num(log.uploads[0].size(), 0, "uploaded object is empty");
    }
}

// ========== commit_async：内联默认实现 + 真异步覆写 ==========

// 8. 默认 commit_async 在发起线程内联执行 —— 本地后端零开销的关键
static void test_commit_async_inline_default() {
    UploadLog log;
    BufferedUploadStream s(recording_upload(&log), recording_cleanup(&log), 4096);
    std::string err;
    s.write_at(0, "mail", 4, err);

    bool called = false;
    bool same_thread = false;
    bool got_ok = false;
    std::string got_err;
    const auto main_id = std::this_thread::get_id();
    s.commit_async([&](bool ok, const std::string& e) {
        called = true;
        same_thread = (std::this_thread::get_id() == main_id);
        got_ok = ok;
        got_err = e;
    });
    expect_true(called, "default commit_async fires synchronously (inline)");
    expect_true(same_thread, "default impl runs callback on calling thread");
    expect_true(got_ok, "inline commit_async reports ok");
    expect_true(got_err.empty(), "success carries empty error");
    expect_num(log.uploads.size(), 1, "commit performed before callback");
}

// 9. 真异步覆写：回调来自 provider 线程，MailBodyWriter::commit_async 原样透传。
//    这是远程后端将来覆写 commit_async 后 DATA_END 回调的形状。
class FakeAsyncProviderStream : public IWriteStream {
public:
    explicit FakeAsyncProviderStream(bool succeed) : succeed_(succeed) {}

    bool write_at(std::uint64_t, const char* data, std::size_t size, std::string&) override {
        buffered_.append(data, size);
        return true;
    }
    bool commit(std::string& error) override {
        if (!succeed_) { error = "injected async failure"; return false; }
        committed_ = true;
        return true;
    }
    void abort() noexcept override {}

    // 模拟真异步后端：把 commit 挪到别的线程执行，回调在 provider 线程触发
    void commit_async(CommitCallback cb) override {
        std::thread([this, cb = std::move(cb)]() mutable {
            std::string error;
            const bool ok = commit(error);
            cb(ok, ok ? std::string() : error);
        }).detach();
    }

    std::string buffered_;
    bool committed_ = false;
    bool succeed_;
};

static void test_commit_async_deferred_from_provider_thread() {
    auto stream = std::make_unique<FakeAsyncProviderStream>(true);
    auto* raw = stream.get();
    MailBodyWriter w(std::move(stream), 64);
    std::string err;
    w.write("body", 4, err);

    std::promise<std::pair<bool, std::string>> done;
    auto fut = done.get_future();
    const auto main_id = std::this_thread::get_id();
    w.commit_async([&done, main_id](bool ok, const std::string& e) {
        // 回调线程契约：真异步实现允许在 provider 线程触发 —— 这里就断言它确实
        // 不在发起线程上（发起线程此刻正阻塞在 future::get）
        expect_true(std::this_thread::get_id() != main_id,
                    "deferred commit_async fires on provider thread");
        done.set_value({ok, e});
    });
    const auto result = fut.get();
    expect_true(result.first, "deferred commit reports ok");
    expect_true(result.second.empty(), "deferred success carries empty error");
    expect_true(raw->committed_, "underlying stream committed");
    expect_true(w.committed(), "writer marked committed");

    // 失败路径同样从 provider 线程回调
    MailBodyWriter w2(std::make_unique<FakeAsyncProviderStream>(false), 64);
    w2.write("body", 4, err);
    std::promise<std::pair<bool, std::string>> done2;
    auto fut2 = done2.get_future();
    w2.commit_async([&done2](bool ok, const std::string& e) {
        done2.set_value({ok, e});
    });
    const auto result2 = fut2.get();
    expect_true(!result2.first, "deferred failure reports !ok");
    expect_str(result2.second, "injected async failure", "failure carries error");
    expect_true(w2.failed(), "writer marked failed after deferred failure");
}

// 10. MailBodyWriter::commit_async 的前置状态检查内联完成：
//     已 failed / 已 committed 的 writer 立刻回调，不触碰底层流
static void test_commit_async_state_short_circuits() {
    std::string err;
    {
        UploadLog log;
        MailBodyWriter w(std::make_unique<BufferedUploadStream>(
                             recording_upload(&log), recording_cleanup(&log), 4096),
                         64);
        w.write("x", 1, err);
        expect_true(w.commit(err), "commit ok");
        bool called = false; bool ok = false;
        w.commit_async([&](bool o, const std::string&) { called = true; ok = o; });
        expect_true(called, "committed writer callbacks inline");
        expect_true(ok, "committed writer reports ok");
    }
    {
        // 先把流写失败（超容量），commit_async 必须内联回 false
        MailBodyWriter w(std::make_unique<BufferedUploadStream>(
                             [](const char*, std::size_t, std::string&) { return true; },
                             []() {}, 10),
                         8);
        expect_true(!w.write(make_payload(20, 3).data(), 20, err), "force failure");
        expect_true(w.failed(), "writer failed");
        bool called = false; bool ok = true;
        w.commit_async([&](bool o, const std::string&) { called = true; ok = o; });
        expect_true(called, "failed writer callbacks inline");
        expect_true(!ok, "failed writer reports !ok");
    }
}

int main() {
    std::printf("buffered_upload_stream_test\n");
    test_single_upload_at_commit();
    test_out_of_order_rejected();
    test_capacity_limit();
    test_abort_without_upload_attempt();
    test_failed_commit_cleans_up();
    test_commit_idempotent();
    test_empty_object_still_uploads();
    test_commit_async_inline_default();
    test_commit_async_deferred_from_provider_thread();
    test_commit_async_state_short_circuits();

    std::printf("  pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
