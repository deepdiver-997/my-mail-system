#include "mail_system/back/storage/local_file_write_stream.h"

#include <cerrno>
#include <cstring>
#include <filesystem>

#include <fcntl.h>
#include <unistd.h>

namespace mail_system {
namespace storage {

namespace {

std::string errno_message(const char* what, const std::string& path) {
    return std::string(what) + " failed for " + path + ": " + std::strerror(errno);
}

} // namespace

std::unique_ptr<LocalFileWriteStream> LocalFileWriteStream::open(const std::string& path,
                                                                 std::string& error) {
    if (path.empty()) {
        error = "storage key is empty";
        return nullptr;
    }

    try {
        const auto parent = std::filesystem::path(path).parent_path();
        if (!parent.empty() && !std::filesystem::exists(parent)) {
            std::filesystem::create_directories(parent);
        }
    } catch (const std::exception& e) {
        error = std::string("failed to create parent directory for ") + path + ": " + e.what();
        return nullptr;
    }

    // O_TRUNC：open_write 的语义是「从头写这个对象」，不是追加到既有内容后面。
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        error = errno_message("open", path);
        return nullptr;
    }

    return std::unique_ptr<LocalFileWriteStream>(new LocalFileWriteStream(fd, path));
}

LocalFileWriteStream::~LocalFileWriteStream() {
    if (fd_ >= 0) {
        if (committed_) {
            close_fd();
        } else {
            abort();
        }
    }
}

bool LocalFileWriteStream::write_at(std::uint64_t offset,
                                    const char* data,
                                    std::size_t size,
                                    std::string& error) {
    if (fd_ < 0) {
        error = "write stream is closed: " + path_;
        return false;
    }
    if (!data || size == 0) {
        return true;
    }

    std::size_t done = 0;
    while (done < size) {
        const ssize_t n = ::pwrite(fd_,
                                   data + done,
                                   size - done,
                                   static_cast<off_t>(offset + done));
        if (n < 0) {
            if (errno == EINTR) {
                continue; // 被信号打断，重试
            }
            error = errno_message("pwrite", path_);
            return false;
        }
        if (n == 0) {
            error = "pwrite wrote 0 bytes for " + path_;
            return false;
        }
        done += static_cast<std::size_t>(n);
    }
    return true;
}

bool LocalFileWriteStream::commit(std::string& error) {
    if (committed_) {
        return true;
    }
    if (fd_ < 0) {
        error = "write stream is closed: " + path_;
        return false;
    }

    // 只有 fsync 成功才敢让上层回 250：否则邮件可能还停在 page cache 里，
    // 一旦掉电就是「已确认收下但实际丢失」。
    while (::fsync(fd_) != 0) {
        if (errno == EINTR) {
            continue;
        }
        error = errno_message("fsync", path_);
        return false;
    }

    committed_ = true;
    close_fd();
    return true;
}

void LocalFileWriteStream::abort() noexcept {
    close_fd();
    if (committed_ || path_.empty()) {
        return;
    }
    ::unlink(path_.c_str()); // 半成品不留在盘上，避免被后续 MIME 解析当成正常邮件
}

void LocalFileWriteStream::close_fd() noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

} // namespace storage
} // namespace mail_system
