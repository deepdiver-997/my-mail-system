#ifndef MAIL_SYSTEM_STORAGE_LOCAL_FILE_READ_STREAM_H
#define MAIL_SYSTEM_STORAGE_LOCAL_FILE_READ_STREAM_H

#include "mail_system/back/common/mapped_file.h"
#include "mail_system/back/storage/i_read_stream.h"

#include <memory>
#include <utility>

namespace mail_system {
namespace storage {

// 本地文件的只读句柄：mmap 支撑，零拷贝。
//
// mmap 从此只是本地后端的实现细节，上层（FSM）只看到 IReadStream，
// 换成 S3/HDFS 时自动退化成 BufferedReadStream，不需要改调用点。
class MappedReadStream : public IReadStream {
public:
    static std::unique_ptr<MappedReadStream> open(const std::string& path, std::string& error) {
        auto m = MappedFile::open(path, error);
        if (!m) {
            return nullptr;
        }
        return std::unique_ptr<MappedReadStream>(new MappedReadStream(std::move(m)));
    }

    std::string_view view() const override { return mapped_->view(); }
    std::uint64_t size() const override { return mapped_->size(); }

private:
    explicit MappedReadStream(std::unique_ptr<MappedFile> m) : mapped_(std::move(m)) {}

    std::unique_ptr<MappedFile> mapped_;
};

} // namespace storage
} // namespace mail_system

#endif // MAIL_SYSTEM_STORAGE_LOCAL_FILE_READ_STREAM_H
