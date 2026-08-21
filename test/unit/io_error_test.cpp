// IoError 分类回归测试 —— kind 的判定决定 SMTP 回 451 还是 550，
// 分错类要么让对方无意义地重投，要么把本可重投的邮件丢掉。
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <cerrno>
#include <string>

#include "framework/storage/io_error.h"

using pr::IoError;

static int g_pass = 0, g_fail = 0;

static void expect_true(bool c, const char* what) {
    if (c) { g_pass++; }
    else { g_fail++; std::printf("  FAIL %s\n", what); }
}

// errno 的值由 from_errno 现场读取，这里显式设置后立刻调用
static void expect_errno_kind(int e, IoError::Kind want, const char* what) {
    errno = e;
    const auto err = IoError::from_errno("write failed", "/tmp/x");
    const bool ok = (err.kind == want) && err.code == e &&
                    err.message.find("write failed") != std::string::npos &&
                    err.message.find("/tmp/x") != std::string::npos;
    expect_true(ok, what);
}

static void expect_http_kind(int status, IoError::Kind want, const char* what) {
    const auto err = IoError::from_http(status, "GET mail/1");
    expect_true(err.kind == want && err.code == status &&
                err.message.find("GET mail/1") != std::string::npos, what);
}

int main() {
    std::printf("io_error_test\n");

    // 默认构造 = retryable（fail-safe：拿不准时选 451 而不是 550）
    expect_true(IoError{}.retryable(), "default kind is retryable");

    // errno → permanent：权限/只读/超大/名字类，重投结果不变
    expect_errno_kind(EACCES, IoError::Kind::Permanent, "EACCES -> permanent");
    expect_errno_kind(EPERM,  IoError::Kind::Permanent, "EPERM -> permanent");
    expect_errno_kind(EFBIG,  IoError::Kind::Permanent, "EFBIG -> permanent");
    expect_errno_kind(EISDIR, IoError::Kind::Permanent, "EISDIR -> permanent");

    // errno → retryable：空间/IO/资源类，稍后重试可能成功
    expect_errno_kind(ENOSPC, IoError::Kind::Retryable, "ENOSPC -> retryable");
    expect_errno_kind(EDQUOT, IoError::Kind::Retryable, "EDQUOT -> retryable");
    expect_errno_kind(EIO,    IoError::Kind::Retryable, "EIO -> retryable");
    expect_errno_kind(EMFILE, IoError::Kind::Retryable, "EMFILE -> retryable");

    // HTTP：5xx 与 408/429 暂时；其余 4xx 明确拒绝
    expect_http_kind(500, IoError::Kind::Retryable, "HTTP 500 -> retryable");
    expect_http_kind(503, IoError::Kind::Retryable, "HTTP 503 -> retryable");
    expect_http_kind(408, IoError::Kind::Retryable, "HTTP 408 -> retryable");
    expect_http_kind(429, IoError::Kind::Retryable, "HTTP 429 -> retryable");
    expect_http_kind(403, IoError::Kind::Permanent, "HTTP 403 -> permanent");
    expect_http_kind(404, IoError::Kind::Permanent, "HTTP 404 -> permanent");
    expect_http_kind(400, IoError::Kind::Permanent, "HTTP 400 -> permanent");

    // 工厂函数直连
    expect_true(IoError::retryable("x").retryable(), "retryable factory");
    expect_true(!IoError::permanent("x").retryable(), "permanent factory");

    std::printf("  pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
