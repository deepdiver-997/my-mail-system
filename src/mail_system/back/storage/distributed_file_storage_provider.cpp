#include "mail_system/back/storage/distributed_file_storage_provider.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>

namespace mail_system {
namespace storage {

DistributedFileStorageProvider::DistributedFileStorageProvider(std::vector<std::string> storage_roots,
                                                               std::size_t replica_count)
    : replica_count_(replica_count == 0 ? 1 : replica_count) {
    roots_.reserve(storage_roots.size());
    for (const auto& root : storage_roots) {
        if (!root.empty()) {
            roots_.push_back(ensure_trailing_slash(root));
        }
    }
}

bool DistributedFileStorageProvider::ensure_ready(IoError& error) {
    if (roots_.empty()) {
        error = IoError::permanent("distributed storage roots are empty");
        return false;
    }

    try {
        for (const auto& root : roots_) {
            if (!std::filesystem::exists(root)) {
                std::filesystem::create_directories(root);
            }
        }
        return true;
    } catch (const std::exception& e) {
        error = IoError::retryable(e.what());
        return false;
    }
}

std::string DistributedFileStorageProvider::build_mail_body_key(std::uint64_t mail_id) {
    std::ostringstream rel;
    rel << "mail/" << mail_id;
    const auto relative_key = rel.str();
    const auto primary = get_primary_index(relative_key);
    return roots_[primary] + relative_key;
}

std::string DistributedFileStorageProvider::build_attachment_key(std::uint64_t mail_id,
                                                                 const std::string& original_filename) {
    const auto seq = ++attachment_seq_;
    std::ostringstream rel;
    rel << "attachment/" << mail_id << "/" << seq << "_" << sanitize_filename(original_filename);
    const auto relative_key = rel.str();
    const auto primary = get_primary_index(relative_key);
    return roots_[primary] + relative_key;
}

bool DistributedFileStorageProvider::append_binary(const std::string& storage_key,
                                                   const char* data,
                                                   std::size_t size,
                                                   IoError& error) {
    if (storage_key.empty()) {
        error = IoError::permanent("storage key is empty");
        return false;
    }
    if (!data || size == 0) {
        return true;
    }

    const auto relative_key = strip_root_prefix(storage_key);
    const auto indices = pick_replica_indices(relative_key);

    for (const auto idx : indices) {
        const auto target = roots_[idx] + relative_key;
        try {
            const auto parent = std::filesystem::path(target).parent_path();
            if (!parent.empty() && !std::filesystem::exists(parent)) {
                std::filesystem::create_directories(parent);
            }

            std::ofstream out(target, std::ios::binary | std::ios::app);
            if (!out.is_open()) {
                error = IoError::retryable("failed to open replica file for append: " + target);
                return false;
            }
            out.write(data, static_cast<std::streamsize>(size));
            if (!out.good()) {
                error = IoError::retryable("failed to append data to replica: " + target);
                return false;
            }
        } catch (const std::exception& e) {
            error = IoError::retryable(e.what());
            return false;
        }
    }

    return true;
}

bool DistributedFileStorageProvider::read_all(const std::string& storage_key,
                                              std::string& out,
                                              IoError& error) {
    if (storage_key.empty()) {
        error = IoError::permanent("storage key is empty");
        return false;
    }

    const auto relative_key = strip_root_prefix(storage_key);
    const auto indices = pick_replica_indices(relative_key);

    // 任一副本可读即可；全部失败才报错
    for (const auto idx : indices) {
        const auto target = roots_[idx] + relative_key;
        std::ifstream in(target, std::ios::binary);
        if (!in.is_open()) {
            continue;
        }
        out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return true;
    }

    // 全部副本都打不开以对象不存在居多
    error = IoError::permanent("no readable replica for " + storage_key);
    return false;
}

bool DistributedFileStorageProvider::remove_object(const std::string& storage_key,
                                                   IoError& error) {
    if (storage_key.empty()) {
        return true;
    }

    const auto relative_key = strip_root_prefix(storage_key);
    const auto indices = pick_replica_indices(relative_key);

    for (const auto idx : indices) {
        const auto target = roots_[idx] + relative_key;
        if (std::remove(target.c_str()) == 0) {
            continue;
        }
        if (std::filesystem::exists(target)) {
            error = IoError::retryable("failed to remove distributed object: " + target);
            return false;
        }
    }

    return true;
}

std::string DistributedFileStorageProvider::ensure_trailing_slash(const std::string& path) {
    if (path.empty()) {
        return path;
    }
    if (path.back() == '/' || path.back() == '\\') {
        return path;
    }
    return path + "/";
}

std::string DistributedFileStorageProvider::sanitize_filename(const std::string& name) {
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

std::string DistributedFileStorageProvider::strip_root_prefix(const std::string& storage_key) const {
    for (const auto& root : roots_) {
        if (storage_key.rfind(root, 0) == 0) {
            return storage_key.substr(root.size());
        }
    }
    return storage_key;
}

std::size_t DistributedFileStorageProvider::get_primary_index(const std::string& relative_key) const {
    const auto hash_value = std::hash<std::string>{}(relative_key);
    return hash_value % roots_.size();
}

std::vector<std::size_t> DistributedFileStorageProvider::pick_replica_indices(const std::string& relative_key) const {
    std::vector<std::size_t> indices;
    if (roots_.empty()) {
        return indices;
    }

    const auto primary = get_primary_index(relative_key);
    const auto count = std::min(replica_count_, roots_.size());
    indices.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        indices.push_back((primary + i) % roots_.size());
    }
    return indices;
}

} // namespace storage
} // namespace mail_system
