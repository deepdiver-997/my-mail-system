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
#include <memory>
#include <string>
#include <vector>

#include "mail_system/back/storage/buffered_upload_stream.h"
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

int main() {
    std::printf("buffered_upload_stream_test\n");
    test_single_upload_at_commit();
    test_out_of_order_rejected();
    test_capacity_limit();
    test_abort_without_upload_attempt();
    test_failed_commit_cleans_up();
    test_commit_idempotent();
    test_empty_object_still_uploads();

    std::printf("  pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
