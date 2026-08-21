#ifndef PR_FRAMEWORK_STORAGE_IO_ERROR_H
#define PR_FRAMEWORK_STORAGE_IO_ERROR_H

#include <cerrno>
#include <cstring>
#include <string>
#include <utility>

namespace pr {

// 存储层统一错误值：把「发生什么」从字符串提升为结构，调用方据此决定
// 对外行为（SMTP: retryable → 451 让发送方重投；permanent → 550 拒收），
// 而不是一律 451 或去 parse 错误文本。
//
// kind 的判定归属后端：只有后端知道失败来自 errno 还是 HTTP 状态码。
// 分类原则是「重试同样的邮件会不会有不同的结果」：
//   - Retryable（默认，fail-safe）：ENOSPC（空间会释放）、EIO、EMFILE、
//     网络类（ETIMEDOUT/ECONNRESET/...）、HTTP 5xx、curl 传输错误。
//     拿不准时选它 —— 451 最多让对方多投几次，550 会把邮件丢掉。
//   - Permanent：EACCES/EPERM/EROFS/EFBIG/ENAMETOOLONG/EISDIR/ENOTDIR、
//     HTTP 4xx（403/404 拒绝类）。重投结果不会改变。
struct IoError {
    enum class Kind { Retryable, Permanent };

    Kind        kind = Kind::Retryable;
    int         code = 0;     // errno 或 HTTP 状态码，仅诊断用
    std::string message;

    bool retryable() const { return kind == Kind::Retryable; }

    static IoError retryable(std::string msg, int c = 0) {
        return IoError{Kind::Retryable, c, std::move(msg)};
    }
    static IoError permanent(std::string msg, int c = 0) {
        return IoError{Kind::Permanent, c, std::move(msg)};
    }

    // 本地 syscall 失败。errno 取自当前值。
    static IoError from_errno(const std::string& what, const std::string& path = "") {
        const int e = errno;
        const std::string msg = what + (path.empty() ? std::string() : (" " + path)) +
                                ": " + std::strerror(e);
        switch (e) {
            case EACCES: case EPERM: case EROFS: case EFBIG:
            case ENAMETOOLONG: case EISDIR: case ENOTDIR: case EINVAL:
                return permanent(std::move(msg), e);
            default:
                // ENOSPC/EDQUOT/EIO/EMFILE/ENFILE/网络类…… 以及一切未枚举的
                return retryable(std::move(msg), e);
        }
    }

    // 远程 HTTP 失败。5xx 与 408/429 是暂时的；其余 4xx 是明确拒绝。
    static IoError from_http(int status, const std::string& detail) {
        const std::string msg = "HTTP " + std::to_string(status) +
                                (detail.empty() ? std::string() : (": " + detail));
        if (status >= 500 || status == 408 || status == 429) {
            return retryable(std::move(msg), status);
        }
        return permanent(std::move(msg), status);
    }
};

} // namespace pr

#endif // PR_FRAMEWORK_STORAGE_IO_ERROR_H
