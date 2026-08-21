// 正文写入顺序回归测试 — 针对「邮件被写成 M[k:]+M[:k] 环形错位」那类 bug。
//
// 历史现象：生产上 3 封邮件的正文文件开头是报文尾部、报文头躺在文件中间，
// rotation 量分别为 8192/4096/6909，正好等于各自触发异步刷盘时的 chunk 大小。
// 根因是多条写路径都按「追加到文件当前末尾」落盘，落盘顺序 = 调用到达顺序。
//
// 因此本测试的核心断言只有一条：
//   每次 write_at 的 offset 必须等于此前已写入的字节总数。
// 只要写入顺序被打乱，这条断言立刻失败。
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "mail_system/back/storage/i_storage_provider.h"
#include "mail_system/back/storage/local_file_read_stream.h"
#include "mail_system/back/storage/local_file_storage_provider.h"
#include "mail_system/back/storage/local_file_write_stream.h"
#include "mail_system/back/storage/null_storage_provider.h"
#include "mail_system/back/storage/mail_body_writer.h"

using namespace mail_system::storage;

static int g_pass = 0, g_fail = 0;

static void expect_true(bool c, const char* what) {
    if (c) { g_pass++; }
    else { g_fail++; std::printf("  FAIL %s\n", what); }
}
static void expect_str(const std::string& got, const std::string& want, const char* what) {
    if (got == want) { g_pass++; }
    else { g_fail++; std::printf("  FAIL %s: got %zu bytes, want %zu bytes\n",
                                 what, got.size(), want.size()); }
}
static void expect_num(std::uint64_t got, std::uint64_t want, const char* what) {
    if (got == want) { g_pass++; }
    else { g_fail++; std::printf("  FAIL %s: got %llu want %llu\n", what,
        (unsigned long long)got, (unsigned long long)want); }
}

// ========== 记录型假写入流 ==========

struct RecordedWrite {
    std::uint64_t offset;
    std::string   data;
};

class RecordingWriteStream : public IWriteStream {
public:
    struct State {
        std::vector<RecordedWrite> writes;
        bool committed = false;
        bool aborted   = false;
        // 令第 fail_at_write 次 write_at 失败（-1 表示从不失败）
        int  fail_at_write = -1;
        int  write_count = 0;
    };

    explicit RecordingWriteStream(State* st) : st_(st) {}

    bool write_at(std::uint64_t offset, const char* data, std::size_t size,
                  std::string& error) override {
        if (st_->fail_at_write >= 0 && st_->write_count == st_->fail_at_write) {
            st_->write_count++;
            error = "injected write failure";
            return false;
        }
        st_->write_count++;
        st_->writes.push_back(RecordedWrite{offset, std::string(data, size)});
        return true;
    }
    bool commit(std::string&) override { st_->committed = true; return true; }
    void abort() noexcept override { st_->aborted = true; }

private:
    State* st_;
};

// 核心校验：写入必须连续、有序，且拼接结果等于原文
static void expect_contiguous(const RecordingWriteStream::State& st,
                              const std::string& expected,
                              const char* what) {
    std::uint64_t running = 0;
    std::string   rebuilt;
    for (std::size_t i = 0; i < st.writes.size(); ++i) {
        if (st.writes[i].offset != running) {
            g_fail++;
            std::printf("  FAIL %s: write #%zu at offset %llu, expected %llu"
                        " (写入错位！)\n",
                        what, i,
                        (unsigned long long)st.writes[i].offset,
                        (unsigned long long)running);
            return;
        }
        rebuilt += st.writes[i].data;
        running += st.writes[i].data.size();
    }
    g_pass++;
    expect_str(rebuilt, expected, what);
}

static std::string make_payload(std::size_t n, unsigned seed) {
    std::string s;
    s.reserve(n);
    unsigned x = seed;
    for (std::size_t i = 0; i < n; ++i) {
        x = x * 1103515245u + 12345u;               // 确定性伪随机，避免测试不稳定
        s.push_back(static_cast<char>('A' + (x >> 16) % 26));
    }
    return s;
}

// ========== 用例 ==========

// 1. 小于缓冲容量：全部攒在缓冲里，commit 时一次性落盘
static void test_small_writes_single_flush() {
    RecordingWriteStream::State st;
    const std::string payload = "Subject: hi\r\n\r\nbody";
    {
        MailBodyWriter w(std::make_unique<RecordingWriteStream>(&st), 1024);
        std::string err;
        for (char c : payload) {
            expect_true(w.write(&c, 1, err), "small write ok");
        }
        expect_num(w.bytes_total(), payload.size(), "bytes_total counts buffered data");
        expect_true(st.writes.empty(), "nothing flushed before commit");
        expect_true(w.commit(err), "commit ok");
    }
    expect_num(st.writes.size(), 1, "single flush at commit");
    expect_contiguous(st, payload, "small writes contiguous");
    expect_true(st.committed, "stream committed");
    expect_true(!st.aborted, "committed stream not aborted");
}

// 2. 跨越缓冲容量的多次写入 —— 正是历史上写错位的形状
static void test_multi_flush_stays_ordered() {
    RecordingWriteStream::State st;
    const std::string payload = make_payload(50000, 7);
    {
        MailBodyWriter w(std::make_unique<RecordingWriteStream>(&st), 4096);
        std::string err;
        // 按不规则块喂入，模拟 TCP 到达的 chunk
        std::size_t pos = 0;
        const std::size_t chunks[] = {1500, 3000, 700, 8192, 1, 4095, 4096, 9000};
        std::size_t ci = 0;
        while (pos < payload.size()) {
            std::size_t n = chunks[ci++ % (sizeof(chunks) / sizeof(chunks[0]))];
            n = std::min(n, payload.size() - pos);
            expect_true(w.write(payload.data() + pos, n, err), "chunk write ok");
            pos += n;
        }
        expect_true(w.commit(err), "commit ok");
    }
    expect_true(st.writes.size() > 1, "multiple flushes happened");
    expect_contiguous(st, payload, "multi-flush contiguous (anti-rotation)");
}

// 3. 单块大于缓冲容量：直穿，偏移仍连续
static void test_oversized_chunk_passthrough() {
    RecordingWriteStream::State st;
    const std::string head = make_payload(100, 1);
    const std::string huge = make_payload(70000, 2);   // > 64KB 默认缓冲
    {
        MailBodyWriter w(std::make_unique<RecordingWriteStream>(&st), 8192);
        std::string err;
        expect_true(w.write(head.data(), head.size(), err), "head write ok");
        expect_true(w.write(huge.data(), huge.size(), err), "huge write ok");
        expect_true(w.commit(err), "commit ok");
    }
    expect_contiguous(st, head + huge, "oversized chunk contiguous");
    expect_num(st.writes.size(), 2, "head flushed then huge passed through");
}

// 4. 未 commit 就析构 → abort，半成品不留存
static void test_destructor_aborts_uncommitted() {
    RecordingWriteStream::State st;
    {
        MailBodyWriter w(std::make_unique<RecordingWriteStream>(&st), 1024);
        std::string err;
        w.write("partial", 7, err);
    }
    expect_true(st.aborted, "uncommitted writer aborts on destruction");
    expect_true(!st.committed, "uncommitted writer never commits");
}

// 5. abort 幂等；commit 之后析构不再 abort
static void test_abort_idempotent() {
    RecordingWriteStream::State st;
    {
        MailBodyWriter w(std::make_unique<RecordingWriteStream>(&st), 1024);
        std::string err;
        w.write("x", 1, err);
        expect_true(w.commit(err), "commit ok");
        w.abort();
        w.abort();
    }
    expect_true(!st.aborted, "committed stream is not aborted afterwards");
}

// 6. 写失败后不得再假装成功 —— 失败必须一路传到 commit
static void test_failure_propagates() {
    RecordingWriteStream::State st;
    st.fail_at_write = 0;                    // 第一次落盘就失败
    MailBodyWriter w(std::make_unique<RecordingWriteStream>(&st), 64);
    std::string err;
    const std::string big = make_payload(200, 3);
    const bool wrote = w.write(big.data(), big.size(), err);
    expect_true(!wrote, "write reports failure");
    expect_true(!err.empty(), "failure fills error string");

    std::string err2;
    expect_true(!w.commit(err2), "commit fails after a failed write");
    expect_true(!st.committed, "stream never committed after failure");
}

// 7. AppendWriteStream 必须拒绝乱序 offset（而不是静默写坏）
class CountingProvider : public IStorageProvider {
public:
    bool ensure_ready(std::string&) override { return true; }
    std::string build_mail_body_key(std::uint64_t id) override { return std::to_string(id); }
    std::string build_attachment_key(std::uint64_t id, const std::string& n) override {
        return std::to_string(id) + n;
    }
    bool append_binary(const std::string&, const char* d, std::size_t n, std::string&) override {
        appended += std::string(d, n);
        return true;
    }
    bool remove_object(const std::string&, std::string&) override { removed = true; return true; }
    bool read_all(const std::string&, std::string& out, std::string&) override {
        out = appended;
        return true;
    }

    std::string appended;
    bool removed = false;
};

static void test_append_stream_rejects_out_of_order() {
    CountingProvider p;
    std::string err;
    auto stream = p.open_write("key", err);
    expect_true(stream != nullptr, "default open_write returns a stream");

    expect_true(stream->write_at(0, "abc", 3, err), "in-order write ok");
    // 跳过一段：正是错位 bug 的形状，必须被拒绝
    expect_true(!stream->write_at(8192, "tail", 4, err), "out-of-order write rejected");
    expect_true(err.find("out-of-order") != std::string::npos, "error names the cause");
    expect_str(p.appended, "abc", "rejected write did not reach the backend");
}

// 8. LocalFileWriteStream 真实文件往返 + abort 清理
static void test_local_file_round_trip() {
    const auto dir = std::filesystem::temp_directory_path() / "mail_body_writer_test";
    std::filesystem::create_directories(dir);
    const auto path = (dir / "roundtrip.eml").string();
    std::filesystem::remove(path);

    const std::string payload = make_payload(30000, 11);
    {
        std::string err;
        auto stream = LocalFileWriteStream::open(path, err);
        expect_true(stream != nullptr, "local stream opens");
        MailBodyWriter w(std::move(stream), 4096);
        std::size_t pos = 0;
        while (pos < payload.size()) {
            const std::size_t n = std::min<std::size_t>(3001, payload.size() - pos);
            expect_true(w.write(payload.data() + pos, n, err), "file chunk write ok");
            pos += n;
        }
        expect_true(w.commit(err), "file commit ok");
    }

    std::ifstream in(path, std::ios::binary);
    std::string got((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    expect_str(got, payload, "file content round-trips byte-for-byte");

    // 未 commit 的写入不得在盘上留下半成品
    const auto abort_path = (dir / "aborted.eml").string();
    std::filesystem::remove(abort_path);
    {
        std::string err;
        auto stream = LocalFileWriteStream::open(abort_path, err);
        expect_true(stream != nullptr, "abort-case stream opens");
        MailBodyWriter w(std::move(stream), 1024);
        w.write("half a message", 14, err);
    }
    expect_true(!std::filesystem::exists(abort_path),
                "aborted body file is removed from disk");

    std::filesystem::remove_all(dir);
}

// ========== 读侧中间层 ==========

// 模拟远程后端：只实现 read_all（没有本地文件可映射），
// open_read / object_size 全部走 IStorageProvider 的默认实现。
class FakeRemoteProvider : public IStorageProvider {
public:
    bool ensure_ready(std::string&) override { return true; }
    std::string build_mail_body_key(std::uint64_t id) override { return "remote://" + std::to_string(id); }
    std::string build_attachment_key(std::uint64_t id, const std::string& n) override {
        return "remote://" + std::to_string(id) + "/" + n;
    }
    bool append_binary(const std::string&, const char* d, std::size_t n, std::string&) override {
        object += std::string(d, n);
        return true;
    }
    bool remove_object(const std::string&, std::string&) override { object.clear(); return true; }
    bool read_all(const std::string& key, std::string& out, std::string& error) override {
        if (key != "remote://42") { error = "no such object: " + key; return false; }
        out = object;
        return true;
    }
    std::string object;
};

// 同一段调用代码，在本地后端和远程后端上必须得到相同结果。
// 这是「抽象是否真的封住了 mmap」的判据：调用方不该知道后端用了什么。
static void test_read_side_backend_agnostic() {
    const auto dir = std::filesystem::temp_directory_path() / "mail_body_writer_test";
    std::filesystem::create_directories(dir);
    const auto path = (dir / "readside.eml").string();
    const std::string payload = make_payload(5000, 21);

    // 本地后端：open_read 应走 mmap
    {
        std::string err;
        auto stream = LocalFileWriteStream::open(path, err);
        MailBodyWriter w(std::move(stream), 1024);
        expect_true(w.write(payload.data(), payload.size(), err), "local write ok");
        expect_true(w.commit(err), "local commit ok");
    }
    LocalFileStorageProvider local(dir.string() + "/", dir.string() + "/");

    std::string err;
    auto local_stream = local.open_read(path, err);
    expect_true(local_stream != nullptr, "local open_read ok");
    if (local_stream) {
        expect_str(std::string(local_stream->view()), payload, "local view == payload");
        expect_num(local_stream->size(), payload.size(), "local size matches");
    }
    std::uint64_t lsz = 0;
    expect_true(local.object_size(path, lsz, err), "local object_size ok");
    expect_num(lsz, payload.size(), "local object_size value");

    std::string lbuf;
    expect_true(local.read_all(path, lbuf, err), "local read_all ok");
    expect_str(lbuf, payload, "local read_all == payload");

    // 远程后端：同样的调用，走默认实现（下载进堆缓冲）
    FakeRemoteProvider remote;
    remote.object = payload;
    auto remote_stream = remote.open_read("remote://42", err);
    expect_true(remote_stream != nullptr, "remote open_read ok (default impl)");
    if (remote_stream) {
        expect_str(std::string(remote_stream->view()), payload, "remote view == payload");
        expect_num(remote_stream->size(), payload.size(), "remote size matches");
    }
    std::uint64_t rsz = 0;
    expect_true(remote.object_size("remote://42", rsz, err), "remote object_size ok (default impl)");
    expect_num(rsz, payload.size(), "remote object_size value");

    // 两个后端的只读视图内容必须一致
    if (local_stream && remote_stream) {
        expect_str(std::string(local_stream->view()), std::string(remote_stream->view()),
                   "local view == remote view (后端无关)");
    }

    // 失败路径要有明确 error，不能静默返回空内容
    std::string missing_err;
    expect_true(remote.open_read("remote://999", missing_err) == nullptr,
                "remote open_read fails for missing object");
    expect_true(!missing_err.empty(), "missing object fills error");

    std::string local_miss_err;
    expect_true(local.open_read(dir.string() + "/nope.eml", local_miss_err) == nullptr,
                "local open_read fails for missing file");
    expect_true(!local_miss_err.empty(), "missing file fills error");

    std::filesystem::remove_all(dir);
}

// Null 后端读必须明确失败，而不是返回空内容冒充成功
static void test_null_provider_read_fails_loudly() {
    NullStorageProvider np;
    std::string out = "sentinel", err;
    expect_true(!np.read_all("/dev/null/1", out, err), "null read_all fails");
    expect_true(!err.empty(), "null read_all fills error");
    expect_true(np.open_read("/dev/null/1", err) == nullptr, "null open_read returns nullptr");
}

int main() {
    std::printf("mail_body_writer_test\n");
    test_small_writes_single_flush();
    test_multi_flush_stays_ordered();
    test_oversized_chunk_passthrough();
    test_destructor_aborts_uncommitted();
    test_abort_idempotent();
    test_failure_propagates();
    test_append_stream_rejects_out_of_order();
    test_local_file_round_trip();
    test_read_side_backend_agnostic();
    test_null_provider_read_fails_loudly();

    std::printf("  pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
