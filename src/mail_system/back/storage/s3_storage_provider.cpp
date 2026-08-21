#include "mail_system/back/storage/s3_storage_provider.h"

#include "framework/storage/buffered_upload_stream.h"

#include <curl/curl.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <string>
#include <cstring>

namespace mail_system {
namespace storage {

// ========== libcurl helpers =================================================

namespace {

struct HttpResponse {
    long status_code = 0;
    std::string body;
};

size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

size_t read_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* src = static_cast<std::pair<const char*, std::size_t>*>(userdata);
    const std::size_t total = size * nmemb;
    const std::size_t remaining = src->second;
    const std::size_t to_copy = (total < remaining) ? total : remaining;
    if (to_copy == 0) return 0;
    std::memcpy(ptr, src->first, to_copy);
    src->first += to_copy;
    src->second -= to_copy;
    return to_copy;
}

bool curl_perform(CURL* curl, const std::string& url, const std::string& method,
                  long timeout_ms, const char* body, std::size_t body_size,
                  struct curl_slist* headers, HttpResponse& resp, IoError& error) {
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp.body);

    if (headers) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }

    // For PUT, set upload data. body 非空但 size 为 0 也走这里：
    // 空对象 PUT 必须是 Content-Length: 0 的合法上传。
    std::pair<const char*, std::size_t> upload_src{nullptr, 0};
    if (method == "PUT" && body) {
        upload_src = {body, body_size};
        curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(curl, CURLOPT_READFUNCTION, read_callback);
        curl_easy_setopt(curl, CURLOPT_READDATA, &upload_src);
        curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE,
                         static_cast<curl_off_t>(body_size));
    }

    const CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        // 传输层失败（连接/超时/TLS）：重试可能成功
        error = IoError::retryable(std::string("curl: ") + curl_easy_strerror(rc));
        return false;
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.status_code);
    return true;
}

} // anonymous namespace

// ========== AWS Signature V4 ===============================================

std::string S3StorageProvider::hex_encode(const unsigned char* data, std::size_t len) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < len; ++i)
        oss << std::setw(2) << static_cast<int>(data[i]);
    return oss.str();
}

std::string S3StorageProvider::sha256_hex(const std::string& data) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), hash);
    return hex_encode(hash, SHA256_DIGEST_LENGTH);
}

std::string S3StorageProvider::sha256_hex(const char* data, std::size_t size) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(data), size, hash);
    return hex_encode(hash, SHA256_DIGEST_LENGTH);
}

std::string S3StorageProvider::hmac_sha256(const std::string& key, const std::string& msg) {
    unsigned char result[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(msg.data()), msg.size(),
         result, &len);
    return {reinterpret_cast<char*>(result), len};
}

std::string S3StorageProvider::hmac_sha256_hex(const std::string& key, const std::string& msg) {
    auto raw = hmac_sha256(key, msg);
    return hex_encode(reinterpret_cast<const unsigned char*>(raw.data()), raw.size());
}

std::string S3StorageProvider::iso8601_date() const {
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
    gmtime_r(&tt, &tm);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y%m%d");
    return oss.str();
}

std::string S3StorageProvider::iso8601_basic() const {
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
    gmtime_r(&tt, &tm);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y%m%dT%H%M%SZ");
    return oss.str();
}

std::string S3StorageProvider::sign_request(
    const std::string& method,
    const std::string& key,
    const std::string& payload_hash,
    const std::map<std::string, std::string>& extra_headers) const {

    const std::string amz_date    = iso8601_basic();
    const std::string date_stamp  = iso8601_date();
    const std::string service     = "s3";
    const std::string credential_scope = date_stamp + "/" + region_ + "/" + service + "/aws4_request";

    // --- canonical request ---
    std::ostringstream canonical;
    canonical << method << '\n';
    canonical << '/' << key << '\n';           // URI
    canonical << '\n';                          // query string (none)
    // headers (sorted by lowercase name)
    std::ostringstream signed_headers;
    if (!extra_headers.empty()) {
        for (const auto& [h, v] : extra_headers) {
            std::string lower = h;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            canonical << lower << ':' << v << '\n';
            if (!signed_headers.str().empty()) signed_headers << ';';
            signed_headers << lower;
        }
    }
    canonical << '\n';
    canonical << signed_headers.str() << '\n';
    canonical << payload_hash;

    // --- string to sign ---
    const std::string canonical_hash = sha256_hex(canonical.str());
    std::ostringstream sts;
    sts << "AWS4-HMAC-SHA256\n";
    sts << amz_date << '\n';
    sts << credential_scope << '\n';
    sts << canonical_hash;

    // --- signing key ---
    const std::string k_date    = hmac_sha256("AWS4" + secret_key_, date_stamp);
    const std::string k_region  = hmac_sha256(k_date, region_);
    const std::string k_service = hmac_sha256(k_region, service);
    const std::string k_signing = hmac_sha256(k_service, "aws4_request");
    const std::string signature = hmac_sha256_hex(k_signing, sts.str());

    // --- authorization header ---
    std::ostringstream auth;
    auth << "AWS4-HMAC-SHA256 "
         << "Credential=" << access_key_ << '/' << credential_scope << ", "
         << "SignedHeaders=" << signed_headers.str() << ", "
         << "Signature=" << signature;
    return auth.str();
}

// ========== S3 HTTP operations =============================================

bool S3StorageProvider::s3_get(const std::string& key, std::string& body,
                                IoError& error) {
    CURL* curl = curl_easy_init();
    if (!curl) { error = IoError::retryable("curl init failed"); return false; }

    const std::string url = use_path_style_
        ? endpoint_ + "/" + bucket_ + "/" + key
        : endpoint_ + "/" + key;

    const std::string payload_hash = sha256_hex("");
    const std::string amz_date = iso8601_basic();
    const std::string date_stamp = iso8601_date();

    std::map<std::string, std::string> extra_headers;
    extra_headers["x-amz-content-sha256"] = payload_hash;
    extra_headers["x-amz-date"] = amz_date;
    if (!access_key_.empty()) {
        extra_headers["host"] = bucket_ + "." + endpoint_;
    } else {
        // MinIO without auth — host header not needed for signing
    }

    const std::string auth = sign_request("GET", key, payload_hash, extra_headers);
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("Authorization: " + auth).c_str());
    headers = curl_slist_append(headers, ("x-amz-content-sha256: " + payload_hash).c_str());
    headers = curl_slist_append(headers, ("x-amz-date: " + amz_date).c_str());

    HttpResponse resp;
    bool ok = curl_perform(curl, url, "GET", timeout_ms_, nullptr, 0, headers, resp, error);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (!ok) return false;
    if (resp.status_code == 404) { body.clear(); return true; }  // not found = empty
    if (resp.status_code >= 200 && resp.status_code < 300) {
        body = std::move(resp.body);
        return true;
    }
    error = IoError::from_http(resp.status_code, "GET " + key + ": " + resp.body);
    return false;
}

bool S3StorageProvider::s3_put(const std::string& key, const char* data,
                                std::size_t size, IoError& error) {
    CURL* curl = curl_easy_init();
    if (!curl) { error = IoError::retryable("curl init failed"); return false; }

    const std::string url = use_path_style_
        ? endpoint_ + "/" + bucket_ + "/" + key
        : endpoint_ + "/" + key;

    const std::string payload_hash = sha256_hex(data, size);
    const std::string amz_date = iso8601_basic();

    std::map<std::string, std::string> extra_headers;
    extra_headers["x-amz-content-sha256"] = payload_hash;
    extra_headers["x-amz-date"] = amz_date;

    const std::string auth = sign_request("PUT", key, payload_hash, extra_headers);
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("Authorization: " + auth).c_str());
    headers = curl_slist_append(headers, ("x-amz-content-sha256: " + payload_hash).c_str());
    headers = curl_slist_append(headers, ("x-amz-date: " + amz_date).c_str());
    headers = curl_slist_append(headers, "Content-Type: application/octet-stream");

    HttpResponse resp;
    bool ok = curl_perform(curl, url, "PUT", timeout_ms_, data, size, headers, resp, error);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (!ok) return false;
    if (resp.status_code >= 200 && resp.status_code < 300) return true;
    error = IoError::from_http(resp.status_code, "PUT " + key + ": " + resp.body);
    return false;
}

bool S3StorageProvider::s3_delete(const std::string& key, IoError& error) {
    CURL* curl = curl_easy_init();
    if (!curl) { error = IoError::retryable("curl init failed"); return false; }

    const std::string url = use_path_style_
        ? endpoint_ + "/" + bucket_ + "/" + key
        : endpoint_ + "/" + key;

    const std::string payload_hash = sha256_hex("");
    const std::string amz_date = iso8601_basic();

    std::map<std::string, std::string> extra_headers;
    extra_headers["x-amz-content-sha256"] = payload_hash;
    extra_headers["x-amz-date"] = amz_date;

    const std::string auth = sign_request("DELETE", key, payload_hash, extra_headers);
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("Authorization: " + auth).c_str());
    headers = curl_slist_append(headers, ("x-amz-content-sha256: " + payload_hash).c_str());
    headers = curl_slist_append(headers, ("x-amz-date: " + amz_date).c_str());

    HttpResponse resp;
    bool ok = curl_perform(curl, url, "DELETE", timeout_ms_, nullptr, 0, headers, resp, error);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (!ok) return false;
    if ((resp.status_code >= 200 && resp.status_code < 300) || resp.status_code == 404) return true;
    error = IoError::from_http(resp.status_code, "DELETE " + key + ": " + resp.body);
    return false;
}

// ========== IStorageProvider interface ======================================

S3StorageProvider::S3StorageProvider(std::string endpoint,
                                     std::string bucket,
                                     std::string access_key,
                                     std::string secret_key,
                                     std::string region,
                                     long timeout_ms,
                                     bool use_path_style,
                                     std::size_t max_write_buffer_bytes)
    : endpoint_(std::move(endpoint))
    , bucket_(std::move(bucket))
    , access_key_(std::move(access_key))
    , secret_key_(std::move(secret_key))
    , region_(std::move(region))
    , timeout_ms_(timeout_ms)
    , use_path_style_(use_path_style)
    , max_write_buffer_bytes_(max_write_buffer_bytes == 0
                                  ? kDefaultWriteBufferBytes
                                  : max_write_buffer_bytes)
{
    // strip trailing slash
    while (!endpoint_.empty() && endpoint_.back() == '/')
        endpoint_.pop_back();
}

bool S3StorageProvider::ensure_ready(IoError& error) {
    // HEAD bucket — 验证连通性和权限
    CURL* curl = curl_easy_init();
    if (!curl) { error = IoError::retryable("curl init failed"); return false; }

    const std::string url = endpoint_ + "/" + bucket_;
    const std::string payload_hash = sha256_hex("");
    const std::string amz_date = iso8601_basic();

    std::map<std::string, std::string> extra_headers;
    extra_headers["x-amz-content-sha256"] = payload_hash;
    extra_headers["x-amz-date"] = amz_date;

    const std::string auth = sign_request("HEAD", "", payload_hash, extra_headers);
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("Authorization: " + auth).c_str());
    headers = curl_slist_append(headers, ("x-amz-content-sha256: " + payload_hash).c_str());
    headers = curl_slist_append(headers, ("x-amz-date: " + amz_date).c_str());

    HttpResponse resp;
    bool ok = curl_perform(curl, url, "HEAD", timeout_ms_, nullptr, 0, headers, resp, error);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (!ok) return false;
    if (resp.status_code >= 200 && resp.status_code < 300) return true;
    if (resp.status_code == 404) {
        error = IoError::permanent("S3 bucket '" + bucket_ + "' not found on " + endpoint_);
        return false;
    }
    if (resp.status_code == 403) {
        error = IoError::permanent("S3 access denied for bucket '" + bucket_ + "' — check credentials");
        return false;
    }
    error = IoError::from_http(resp.status_code, "HEAD bucket " + bucket_);
    return false;
}

std::string S3StorageProvider::build_mail_body_key(std::uint64_t mail_id) {
    return "mail/" + std::to_string(mail_id);
}

std::string S3StorageProvider::build_attachment_key(std::uint64_t mail_id,
                                                     const std::string& filename) {
    auto seq = attachment_seq_.fetch_add(1, std::memory_order_relaxed);
    return "attachments/" + std::to_string(mail_id) + "/"
           + std::to_string(seq) + "_" + filename;
}

bool S3StorageProvider::append_binary(const std::string& key,
                                       const char* data,
                                       std::size_t size,
                                       IoError& error) {
    // GET 已有内容 + 拼接新数据 + PUT 全量。每次调用都是两整次对象传输，
    // 只适合一次性写完整内容的调用方（如 IMAP APPEND）；
    // 流式写入必须走 open_write（内存缓冲 + commit 时单次 PUT）。
    std::string existing;
    if (!s3_get(key, existing, error)) return false;

    existing.append(data, size);
    return s3_put(key, existing.data(), existing.size(), error);
}

std::unique_ptr<IWriteStream> S3StorageProvider::open_write(const std::string& key,
                                                            IoError& error) {
    if (key.empty()) {
        error = IoError::permanent("s3 key is empty");
        return nullptr;
    }
    return std::make_unique<BufferedUploadStream>(
        [this, key](const char* data, std::size_t size, IoError& err) {
            // PUT 是整对象原子替换：覆盖同 key 旧对象，无需先 GET/DELETE。
            return s3_put(key, data, size, err);
        },
        [this, key]() {
            IoError ignored;
            s3_delete(key, ignored);
        },
        max_write_buffer_bytes_);
}

bool S3StorageProvider::read_all(const std::string& key,
                                 std::string& out,
                                 IoError& error) {
    if (key.empty()) {
        error = IoError::permanent("s3 key is empty");
        return false;
    }
    // 复用已有的签名 GET
    return s3_get(key, out, error);
}

bool S3StorageProvider::remove_object(const std::string& key, IoError& error) {
    return s3_delete(key, error);
}

} // namespace storage
} // namespace mail_system
