#include "mail_system/back/storage/local_file_storage_provider.h"

#include "framework/storage/local_file_read_stream.h"
#include "framework/storage/local_file_write_stream.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace mail_system {
namespace storage {

LocalFileStorageProvider::LocalFileStorageProvider(std::string mail_root, std::string attachment_root)
    : mail_root_(ensure_trailing_slash(mail_root)),
      attachment_root_(ensure_trailing_slash(attachment_root)) {}

bool LocalFileStorageProvider::ensure_ready(std::string& error) {
    try {
        if (!mail_root_.empty() && !std::filesystem::exists(mail_root_)) {
            std::filesystem::create_directories(mail_root_);
        }
        if (!attachment_root_.empty() && !std::filesystem::exists(attachment_root_)) {
            std::filesystem::create_directories(attachment_root_);
        }
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

std::string LocalFileStorageProvider::build_mail_body_key(std::uint64_t mail_id) {
    std::ostringstream oss;
    oss << mail_root_ << mail_id;
    return oss.str();
}

std::string LocalFileStorageProvider::build_attachment_key(std::uint64_t mail_id,
                                                           const std::string& original_filename) {
    const auto seq = ++attachment_seq_;
    std::ostringstream oss;
    oss << attachment_root_ << mail_id << "_" << seq << "_" << sanitize_filename(original_filename);
    return oss.str();
}

bool LocalFileStorageProvider::append_binary(const std::string& storage_key,
                                             const char* data,
                                             std::size_t size,
                                             std::string& error) {
    if (storage_key.empty()) {
        error = "storage key is empty";
        return false;
    }
    if (!data || size == 0) {
        return true;
    }

    try {
        const auto parent = std::filesystem::path(storage_key).parent_path();
        if (!parent.empty() && !std::filesystem::exists(parent)) {
            std::filesystem::create_directories(parent);
        }

        std::ofstream out(storage_key, std::ios::binary | std::ios::app);
        if (!out.is_open()) {
            error = "failed to open file for append: " + storage_key;
            return false;
        }
        out.write(data, static_cast<std::streamsize>(size));
        if (!out.good()) {
            error = "failed to append data: " + storage_key;
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

std::unique_ptr<IWriteStream> LocalFileStorageProvider::open_write(const std::string& storage_key,
                                                                   std::string& error) {
    return LocalFileWriteStream::open(storage_key, error);
}

bool LocalFileStorageProvider::read_all(const std::string& storage_key,
                                        std::string& out,
                                        std::string& error) {
    if (storage_key.empty()) {
        error = "storage key is empty";
        return false;
    }
    std::ifstream in(storage_key, std::ios::binary);
    if (!in.is_open()) {
        error = "failed to open for read: " + storage_key;
        return false;
    }
    out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return true;
}

std::unique_ptr<IReadStream> LocalFileStorageProvider::open_read(const std::string& storage_key,
                                                                  std::string& error) {
    return MappedReadStream::open(storage_key, error);
}

bool LocalFileStorageProvider::object_size(const std::string& storage_key,
                                           std::uint64_t& size,
                                           std::string& error) {
    if (storage_key.empty()) {
        error = "storage key is empty";
        return false;
    }
    std::error_code ec;
    const auto sz = std::filesystem::file_size(storage_key, ec);
    if (ec) {
        error = "file_size failed for " + storage_key + ": " + ec.message();
        return false;
    }
    size = static_cast<std::uint64_t>(sz);
    return true;
}

bool LocalFileStorageProvider::remove_object(const std::string& storage_key,
                                             std::string& error) {
    if (storage_key.empty()) {
        return true;
    }

    if (std::remove(storage_key.c_str()) == 0) {
        return true;
    }

    if (std::filesystem::exists(storage_key)) {
        error = "failed to remove object: " + storage_key;
        return false;
    }
    return true;
}

std::string LocalFileStorageProvider::ensure_trailing_slash(const std::string& path) {
    if (path.empty()) {
        return path;
    }
    if (path.back() == '/' || path.back() == '\\') {
        return path;
    }
    return path + "/";
}

std::string LocalFileStorageProvider::sanitize_filename(const std::string& name) {
    if (name.empty()) {
        return "attachment";
    }

    std::string out;
    out.reserve(name.size());
    for (const auto c : name) {
        if (c == '/' || c == '\\' || c == ' ' || c == ':' || c == '*') {
            out.push_back('_');
        } else {
            out.push_back(c);
        }
    }
    return out.empty() ? std::string("attachment") : out;
}

} // namespace storage
} // namespace mail_system
