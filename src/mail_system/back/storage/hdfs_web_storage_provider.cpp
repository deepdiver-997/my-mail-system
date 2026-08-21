#include "mail_system/back/storage/hdfs_web_storage_provider.h"

#include "framework/storage/buffered_upload_stream.h"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>

namespace mail_system {
namespace storage {

namespace {

struct HttpResponse {
    long status_code = 0;
    std::string body;
    std::string redirect_location;
};

size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    const size_t total = size * nmemb;
    out->append(ptr, total);
    return total;
}

size_t header_callback(char* buffer, size_t size, size_t nitems, void* userdata) {
    auto* location = static_cast<std::string*>(userdata);
    const size_t total = size * nitems;
    std::string line(buffer, total);
    const std::string key = "Location:";
    if (line.rfind(key, 0) == 0) {
        auto value = line.substr(key.size());
        value.erase(value.begin(),
                    std::find_if(value.begin(), value.end(), [](unsigned char ch) {
                        return !std::isspace(ch);
                    }));
        while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) {
            value.pop_back();
        }
        *location = value;
    }
    return total;
}

bool http_request(const std::string& method,
                  const std::string& url,
                  long timeout_ms,
                  const char* payload,
                  std::size_t payload_size,
                  HttpResponse& response,
                  IoError& error) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        error = IoError::retryable("curl_easy_init failed");
        return false;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response.redirect_location);

    if (payload && payload_size > 0) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload_size));
    }

    const CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        // 传输层失败（连接/超久）：重试可能成功
        error = IoError::retryable(curl_easy_strerror(rc));
        curl_easy_cleanup(curl);
        return false;
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status_code);
    curl_easy_cleanup(curl);
    return true;
}

std::string get_parent_dir(const std::string& path) {
    const auto pos = path.find_last_of('/');
    if (pos == std::string::npos) {
        return std::string();
    }
    return path.substr(0, pos);
}

} // namespace

HdfsWebStorageProvider::HdfsWebStorageProvider(std::string endpoint,
                                               std::string base_path,
                                               std::string user,
                                               std::size_t replica_count,
                                               long timeout_ms,
                                               std::size_t max_write_buffer_bytes)
    : endpoint_(normalize_endpoint(endpoint)),
      base_path_(normalize_base_path(base_path)),
      user_(std::move(user)),
      replica_count_(replica_count == 0 ? 1 : replica_count),
      timeout_ms_(timeout_ms <= 0 ? 5000 : timeout_ms),
      max_write_buffer_bytes_(max_write_buffer_bytes == 0
                                  ? kDefaultWriteBufferBytes
                                  : max_write_buffer_bytes) {}

bool HdfsWebStorageProvider::ensure_ready(IoError& error) {
    static std::once_flag curl_init_once;
    std::call_once(curl_init_once, []() {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    });

    if (endpoint_.empty()) {
        error = IoError::permanent("hdfs endpoint is empty");
        return false;
    }

    if (base_path_.empty()) {
        error = IoError::permanent("hdfs base_path is empty");
        return false;
    }

    return webhdfs_mkdirs("", error);
}

std::string HdfsWebStorageProvider::build_mail_body_key(std::uint64_t mail_id) {
    std::ostringstream oss;
    oss << "mail/" << mail_id;
    return oss.str();
}

std::string HdfsWebStorageProvider::build_attachment_key(std::uint64_t mail_id,
                                                         const std::string& original_filename) {
    const auto seq = ++attachment_seq_;
    std::ostringstream oss;
    oss << "attachment/" << mail_id << "/" << seq << "_" << sanitize_filename(original_filename);
    return oss.str();
}

bool HdfsWebStorageProvider::append_binary(const std::string& storage_key,
                                           const char* data,
                                           std::size_t size,
                                           IoError& error) {
    if (storage_key.empty()) {
        error = IoError::permanent("hdfs storage key is empty");
        return false;
    }
    if (!data || size == 0) {
        return true;
    }

    const auto parent_dir = get_parent_dir(storage_key);
    if (!ensure_remote_directory(parent_dir, error)) {
        return false;
    }

    if (webhdfs_append(storage_key, data, size, error)) {
        return true;
    }

    // File likely does not exist; fallback to create with payload.
    return webhdfs_create_with_payload(storage_key, data, size, error);
}

bool HdfsWebStorageProvider::read_all(const std::string& storage_key,
                                      std::string& out,
                                      IoError& error) {
    if (storage_key.empty()) {
        error = IoError::permanent("hdfs storage key is empty");
        return false;
    }

    std::ostringstream open_url;
    open_url << endpoint_ << "/webhdfs/v1" << url_encode(base_path_ + "/" + storage_key)
             << "?op=OPEN&user.name=" << url_encode(user_);

    HttpResponse open_resp;
    if (!http_request("GET", open_url.str(), timeout_ms_, nullptr, 0, open_resp, error)) {
        return false;
    }

    // WebHDFS 的 OPEN 通常先回 307，把正文放在 datanode 上；
    // http_request 不跟随重定向（FOLLOWLOCATION=0），这里手动走第二段。
    if (open_resp.status_code == 307 && !open_resp.redirect_location.empty()) {
        HttpResponse data_resp;
        if (!http_request("GET", open_resp.redirect_location, timeout_ms_, nullptr, 0, data_resp, error)) {
            return false;
        }
        if (data_resp.status_code != 200) {
            error = IoError::from_http(data_resp.status_code,
                                       "webhdfs open(data), body=" + data_resp.body);
            return false;
        }
        out = std::move(data_resp.body);
        return true;
    }

    if (open_resp.status_code == 200) {
        out = std::move(open_resp.body);
        return true;
    }

    error = IoError::from_http(open_resp.status_code, "webhdfs open, body=" + open_resp.body);
    return false;
}

bool HdfsWebStorageProvider::remove_object(const std::string& storage_key,
                                           IoError& error) {
    if (storage_key.empty()) {
        return true;
    }
    return webhdfs_delete(storage_key, error);
}

std::unique_ptr<IWriteStream> HdfsWebStorageProvider::open_write(
    const std::string& storage_key, IoError& error) {
    if (storage_key.empty()) {
        error = IoError::permanent("hdfs storage key is empty");
        return nullptr;
    }
    return std::make_unique<BufferedUploadStream>(
        [this, storage_key](const char* data, std::size_t size, IoError& err) {
            // 流式写入的 key 总是新邮件，直接 CREATE(overwrite=true) 覆盖写，
            // 不走 append_binary 的「先试 APPEND 失败再 CREATE」多两次往返。
            // size==0 也要创建空文件，与本地后端打开即建空文件对齐。
            if (!ensure_remote_directory(get_parent_dir(storage_key), err)) {
                return false;
            }
            return webhdfs_create_with_payload(storage_key, data, size, err);
        },
        [this, storage_key]() {
            IoError ignored;
            webhdfs_delete(storage_key, ignored);
        },
        max_write_buffer_bytes_);
}

std::string HdfsWebStorageProvider::normalize_endpoint(const std::string& endpoint) {
    if (endpoint.empty()) {
        return endpoint;
    }
    std::string out = endpoint;
    while (!out.empty() && out.back() == '/') {
        out.pop_back();
    }
    return out;
}

std::string HdfsWebStorageProvider::normalize_base_path(const std::string& base_path) {
    if (base_path.empty()) {
        return "/";
    }

    std::string out = base_path;
    if (out.front() != '/') {
        out.insert(out.begin(), '/');
    }
    while (out.size() > 1 && out.back() == '/') {
        out.pop_back();
    }
    return out;
}

std::string HdfsWebStorageProvider::sanitize_filename(const std::string& name) {
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

std::string HdfsWebStorageProvider::url_encode(const std::string& value) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;

    for (const auto c : value) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '/' || c == '~') {
            escaped << c;
        } else {
            escaped << '%' << std::uppercase << std::setw(2)
                    << static_cast<int>(static_cast<unsigned char>(c))
                    << std::nouppercase;
        }
    }

    return escaped.str();
}

bool HdfsWebStorageProvider::ensure_remote_directory(const std::string& relative_dir, IoError& error) {
    if (relative_dir.empty()) {
        return true;
    }
    return webhdfs_mkdirs(relative_dir, error);
}

bool HdfsWebStorageProvider::webhdfs_mkdirs(const std::string& relative_dir, IoError& error) {
    std::string path = base_path_;
    if (!relative_dir.empty()) {
        path += "/" + relative_dir;
    }

    std::ostringstream url;
    url << endpoint_ << "/webhdfs/v1" << url_encode(path)
        << "?op=MKDIRS&user.name=" << url_encode(user_);

    HttpResponse response;
    if (!http_request("PUT", url.str(), timeout_ms_, nullptr, 0, response, error)) {
        return false;
    }

    if (response.status_code == 200 || response.status_code == 201) {
        return true;
    }

    error = IoError::from_http(response.status_code, "webhdfs mkdirs, body=" + response.body);
    return false;
}

bool HdfsWebStorageProvider::webhdfs_append(const std::string& relative_path,
                                            const char* data,
                                            std::size_t size,
                                            IoError& error) {
    std::ostringstream open_url;
    open_url << endpoint_ << "/webhdfs/v1" << url_encode(base_path_ + "/" + relative_path)
             << "?op=APPEND&user.name=" << url_encode(user_);

    HttpResponse open_resp;
    if (!http_request("POST", open_url.str(), timeout_ms_, nullptr, 0, open_resp, error)) {
        return false;
    }

    if ((open_resp.status_code != 307 && open_resp.status_code != 200) || open_resp.redirect_location.empty()) {
        error = IoError::from_http(open_resp.status_code, "webhdfs append open, body=" + open_resp.body);
        return false;
    }

    HttpResponse append_resp;
    if (!http_request("POST", open_resp.redirect_location, timeout_ms_, data, size, append_resp, error)) {
        return false;
    }

    if (append_resp.status_code == 200) {
        return true;
    }

    error = IoError::from_http(append_resp.status_code, "webhdfs append, body=" + append_resp.body);
    return false;
}

bool HdfsWebStorageProvider::webhdfs_create_with_payload(const std::string& relative_path,
                                                         const char* data,
                                                         std::size_t size,
                                                         IoError& error) {
    std::ostringstream open_url;
    open_url << endpoint_ << "/webhdfs/v1" << url_encode(base_path_ + "/" + relative_path)
             << "?op=CREATE&overwrite=true&replication=" << replica_count_
             << "&user.name=" << url_encode(user_);

    HttpResponse open_resp;
    if (!http_request("PUT", open_url.str(), timeout_ms_, nullptr, 0, open_resp, error)) {
        return false;
    }

    if ((open_resp.status_code != 307 && open_resp.status_code != 201) || open_resp.redirect_location.empty()) {
        error = IoError::from_http(open_resp.status_code, "webhdfs create open, body=" + open_resp.body);
        return false;
    }

    HttpResponse create_resp;
    if (!http_request("PUT", open_resp.redirect_location, timeout_ms_, data, size, create_resp, error)) {
        return false;
    }

    if (create_resp.status_code == 201 || create_resp.status_code == 200) {
        return true;
    }

    error = IoError::from_http(create_resp.status_code, "webhdfs create, body=" + create_resp.body);
    return false;
}

bool HdfsWebStorageProvider::webhdfs_delete(const std::string& relative_path, IoError& error) {
    std::ostringstream url;
    url << endpoint_ << "/webhdfs/v1" << url_encode(base_path_ + "/" + relative_path)
        << "?op=DELETE&recursive=false&user.name=" << url_encode(user_);

    HttpResponse response;
    if (!http_request("DELETE", url.str(), timeout_ms_, nullptr, 0, response, error)) {
        return false;
    }

    if (response.status_code == 200 || response.status_code == 404) {
        return true;
    }

    error = IoError::from_http(response.status_code, "webhdfs delete, body=" + response.body);
    return false;
}

} // namespace storage
} // namespace mail_system
