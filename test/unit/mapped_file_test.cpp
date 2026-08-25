// MappedFile / MappedReadStream 测试：mmap 只读映射的打开路径与错误路径。
//
// MappedFile 是本地邮件正文读取的核心（MIME 解析直接映射，零拷贝）。
// 此前 mail_body_writer 测试覆盖了写入侧，读取侧无直接测试。
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include "framework/storage/mapped_file.h"
#include "framework/storage/local_file_read_stream.h"
#include "framework/storage/io_error.h"

namespace {

using pr::IoError;
using pr::MappedFile;
using pr::MappedReadStream;

int g_pass = 0, g_fail = 0;
void expect_true(bool c, const char* what) {
    if (c) { g_pass++; }
    else { g_fail++; std::printf("  FAIL %s\n", what); }
}

std::filesystem::path tmp_dir() {
    return std::filesystem::temp_directory_path() / "protorelay_mapped_test";
}

void write_file(const std::filesystem::path& p, const std::string& content) {
    std::ofstream ofs(p, std::ios::trunc);
    ofs << content;
}

} // namespace

int main() {
    std::printf("mapped_file_test\n");
    auto dir = tmp_dir();
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    const std::string content = "From: a@example.com\r\nTo: b@example.com\r\n\r\nbody";
    auto f = dir / "mail.eml";
    write_file(f, content);

    // 1. 现有文件：view 与内容一致，size 正确
    {
        std::string err;
        auto m = MappedFile::open(f.string(), err);
        expect_true(m != nullptr, "open existing file");
        if (m) {
            expect_true(m->size() == content.size(), "size matches");
            expect_true(m->view() == content, "view matches content");
            expect_true(!m->empty(), "non-empty file not empty");
            expect_true(std::memcmp(m->view().data(), content.data(), content.size()) == 0,
                        "raw bytes identical");
        }
    }

    // 2. 空文件：size 0 / empty / 空 view（mmap 长度为 0 会 EINVAL，应走空映射分支）
    {
        auto e = dir / "empty.eml";
        write_file(e, "");
        std::string err;
        auto m = MappedFile::open(e.string(), err);
        expect_true(m != nullptr, "open empty file");
        if (m) {
            expect_true(m->size() == 0 && m->empty(), "empty file size 0");
            expect_true(m->view().empty(), "empty file view empty");
        }
    }

    // 3. 缺失文件：nullptr + 诊断信息
    {
        std::string err;
        auto m = MappedFile::open((dir / "nope.eml").string(), err);
        expect_true(m == nullptr, "missing file returns nullptr");
        expect_true(!err.empty(), "missing file yields diagnostic");
    }

    // 4. 空路径：nullptr + "empty path"
    {
        std::string err;
        auto m = MappedFile::open("", err);
        expect_true(m == nullptr, "empty path returns nullptr");
        expect_true(err == "empty path", "empty path diagnostic exact");
    }

    // 5. MappedReadStream（local_file_read_stream.h 封装）
    {
        IoError err;
        auto rs = MappedReadStream::open(f.string(), err);
        expect_true(rs != nullptr, "read stream opens existing file");
        if (rs) {
            expect_true(rs->size() == content.size(), "read stream size");
            expect_true(rs->view() == content, "read stream view");
        }
        auto bad = MappedReadStream::open((dir / "gone.eml").string(), err);
        expect_true(bad == nullptr, "read stream missing file -> nullptr");
        expect_true(err.retryable(), "mmap failure classified retryable (fail-safe 451)");
    }

    std::filesystem::remove_all(dir);
    std::printf("  pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
