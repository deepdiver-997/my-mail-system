#ifndef PR_FRAMEWORK_STORAGE_MAPPED_FILE_H
#define PR_FRAMEWORK_STORAGE_MAPPED_FILE_H

#include <cerrno>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace pr {

// 只读文件映射（RAII）。
//
// 用途：把整封邮件读进来做 MIME 解析。此前是 ifstream + ostringstream 全量
// 读进堆（每封最多 inbound_mime_parse_limit_bytes = 1 MiB，并发时叠加），
// 而 parse_mime_tree 内部还要再 substr 一次。改成映射之后零拷贝，
// 内存压力交给 page cache —— 扛不住时是换页，而不是把堆撑爆。
//
// SIGBUS 注意：映射期间文件若被截断，访问越界页会触发 SIGBUS 而非返回错误。
// 当前唯一的使用场景是「刚由本进程写完并 fsync 过的邮件正文」，此时没有别的
// 写者，风险为零。若将来要映射可能被并发改写的文件，必须重新评估。
class MappedFile {
public:
    static std::unique_ptr<MappedFile> open(const std::string& path, std::string& error) {
        if (path.empty()) {
            error = "empty path";
            return nullptr;
        }

        const int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) {
            error = std::string("open failed for ") + path + ": " + std::strerror(errno);
            return nullptr;
        }

        struct stat st {};
        if (::fstat(fd, &st) != 0) {
            error = std::string("fstat failed for ") + path + ": " + std::strerror(errno);
            ::close(fd);
            return nullptr;
        }

        const auto size = static_cast<std::size_t>(st.st_size);
        if (size == 0) {
            // mmap 长度为 0 会 EINVAL；空文件直接返回一个空映射
            ::close(fd);
            return std::unique_ptr<MappedFile>(new MappedFile(nullptr, 0));
        }

        void* addr = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        ::close(fd); // 映射建立后 fd 即可关闭，映射本身保持有效
        if (addr == MAP_FAILED) {
            error = std::string("mmap failed for ") + path + ": " + std::strerror(errno);
            return nullptr;
        }

        // 顺序扫描，提示内核预读
        ::madvise(addr, size, MADV_SEQUENTIAL);

        return std::unique_ptr<MappedFile>(new MappedFile(static_cast<const char*>(addr), size));
    }

    ~MappedFile() {
        if (data_ && size_ > 0) {
            ::munmap(const_cast<char*>(data_), size_);
        }
    }

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    std::string_view view() const { return std::string_view(data_ ? data_ : "", size_); }
    std::size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }

private:
    MappedFile(const char* data, std::size_t size) : data_(data), size_(size) {}

    const char* data_;
    std::size_t size_;
};

} // namespace pr

#endif // MAIL_SYSTEM_COMMON_MAPPED_FILE_H
