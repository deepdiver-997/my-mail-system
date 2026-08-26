#include "mail_system/back/mailServer/outbound/outbound_utils.h"
#include "mail_system/back/common/mail_crypto.h"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/sha.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace mail_system {
namespace outbound {
namespace {

std::string sender_domain(const std::string& sender) {
    const auto at_pos = sender.find('@');
    if (at_pos == std::string::npos || at_pos + 1 >= sender.size()) {
        return "outbound.local";
    }
    return sender.substr(at_pos + 1);
}

std::string format_rfc5322_date_utc() {
    const auto now = std::time(nullptr);
    std::tm tm_utc{};
    gmtime_r(&now, &tm_utc);

    char buf[64] = {0};
    if (std::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S +0000", &tm_utc) == 0) {
        return "Thu, 01 Jan 1970 00:00:00 +0000";
    }
    return std::string(buf);
}

std::string build_message_id(const OutboxRecord& record) {
    std::ostringstream oss;
    oss << "<" << record.mail_id << "." << record.id << "@" << sender_domain(record.sender) << ">";
    return oss.str();
}

std::string read_file_all(const std::string& path) {
    if (path.empty()) {
        return {};
    }

    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return {};
    }

    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string normalize_for_smtp_data(const std::string& raw) {
    if (raw.empty()) {
        return {};
    }

    std::ostringstream out;
    std::size_t i = 0;
    while (i < raw.size()) {
        std::size_t j = i;
        while (j < raw.size() && raw[j] != '\n' && raw[j] != '\r') {
            ++j;
        }

        std::string line = raw.substr(i, j - i);
        if (!line.empty() && line.front() == '.') {
            out << '.';
        }
        out << line << "\r\n";

        if (j >= raw.size()) {
            break;
        }

        if (raw[j] == '\r' && (j + 1) < raw.size() && raw[j + 1] == '\n') {
            i = j + 2;
        } else {
            i = j + 1;
        }
    }

    return out.str();
}

bool split_rfc5322_message(const std::string& raw,
                          std::string& header_block,
                          std::string& body_block) {
    std::size_t split_pos = raw.find("\r\n\r\n");
    std::size_t sep_len = 4;
    if (split_pos == std::string::npos) {
        split_pos = raw.find("\n\n");
        sep_len = 2;
    }
    if (split_pos == std::string::npos) {
        return false;
    }

    header_block = raw.substr(0, split_pos);
    body_block = raw.substr(split_pos + sep_len);
    return true;
}

std::unordered_map<std::string, std::string> parse_headers_relaxed_map(const std::string& header_block) {
    std::unordered_map<std::string, std::string> headers;
    std::istringstream iss(header_block);
    std::string line;
    std::string current_key;

    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
            if (!current_key.empty()) {
                headers[current_key] += " " + trim_ascii_ws(line);
            }
            continue;
        }

        const auto pos = line.find(':');
        if (pos == std::string::npos) {
            continue;
        }

        current_key = to_lower_copy(trim_ascii_ws(line.substr(0, pos)));
        headers[current_key] = trim_ascii_ws(line.substr(pos + 1));
    }

    return headers;
}

std::vector<std::string> pick_present_headers_for_dkim(const std::unordered_map<std::string, std::string>& headers) {
    static const std::vector<std::string> preferred = {
        "from",
        "to",
        "subject",
        "date",
        "message-id",
        "mime-version",
        "content-type",
        "content-transfer-encoding",
        "reply-to",
    };

    std::vector<std::string> picked;
    picked.reserve(preferred.size());
    for (const auto& h : preferred) {
        if (headers.find(h) != headers.end()) {
            picked.push_back(h);
        }
    }
    return picked;
}

bool sign_rsa_sha256_base64(const std::string& data,
                            const std::string& private_key_file,
                            std::string& signature_b64,
                            std::string& error_out) {
    FILE* key_fp = std::fopen(private_key_file.c_str(), "r");
    if (!key_fp) {
        error_out = "failed to open DKIM private key file: " + private_key_file;
        return false;
    }

    EVP_PKEY* pkey = PEM_read_PrivateKey(key_fp, nullptr, nullptr, nullptr);
    std::fclose(key_fp);
    if (!pkey) {
        error_out = "failed to parse DKIM private key (PEM)";
        return false;
    }

    EVP_MD_CTX* md_ctx = EVP_MD_CTX_new();
    if (!md_ctx) {
        EVP_PKEY_free(pkey);
        error_out = "failed to allocate EVP_MD_CTX";
        return false;
    }

    bool ok = false;
    do {
        if (EVP_DigestSignInit(md_ctx, nullptr, EVP_sha256(), nullptr, pkey) <= 0) {
            error_out = "EVP_DigestSignInit failed";
            break;
        }
        if (EVP_DigestSignUpdate(md_ctx, data.data(), data.size()) <= 0) {
            error_out = "EVP_DigestSignUpdate failed";
            break;
        }

        std::size_t sig_len = 0;
        if (EVP_DigestSignFinal(md_ctx, nullptr, &sig_len) <= 0 || sig_len == 0) {
            error_out = "EVP_DigestSignFinal size query failed";
            break;
        }

        std::vector<unsigned char> signature(sig_len);
        if (EVP_DigestSignFinal(md_ctx, signature.data(), &sig_len) <= 0) {
            error_out = "EVP_DigestSignFinal failed";
            break;
        }
        signature.resize(sig_len);
        signature_b64 = base64_encode(signature.data(), signature.size());
        if (signature_b64.empty()) {
            error_out = "failed to base64-encode DKIM signature";
            break;
        }
        ok = true;
    } while (false);

    EVP_MD_CTX_free(md_ctx);
    EVP_PKEY_free(pkey);
    return ok;
}

std::string join_header_names_colon(const std::vector<std::string>& names) {
    std::ostringstream oss;
    for (std::size_t i = 0; i < names.size(); ++i) {
        oss << names[i];
        if (i + 1 < names.size()) {
            oss << ":";
        }
    }
    return oss.str();
}

std::string build_dkim_header(const std::unordered_map<std::string, std::string>& headers,
                              const std::string& canonical_body,
                              const OutboundConfig& identity_config,
                              std::string& error_out,
                              const std::vector<std::string>* signed_headers_override = nullptr) {
    const std::string selector = trim_ascii_ws(identity_config.dkim_selector);
    const std::string domain = trim_ascii_ws(identity_config.dkim_domain);
    const std::string key_file = trim_ascii_ws(identity_config.dkim_private_key_file);
    if (selector.empty() || domain.empty() || key_file.empty()) {
        error_out = "dkim config missing selector/domain/private_key_file";
        return {};
    }

    static const std::vector<std::string> default_signed_headers = {
        "from",
        "to",
        "subject",
        "date",
        "message-id",
        "mime-version",
        "content-type",
        "content-transfer-encoding",
        "reply-to",
    };

    const std::vector<std::string>& signed_headers =
        (signed_headers_override && !signed_headers_override->empty())
            ? *signed_headers_override
            : default_signed_headers;

    const std::string body_hash = sha256_base64(canonical_body);
    if (body_hash.empty()) {
        error_out = "failed to compute DKIM body hash";
        return {};
    }

    const std::string signed_header_list = join_header_names_colon(signed_headers);
    const std::string timestamp = std::to_string(static_cast<long long>(std::time(nullptr)));

    std::ostringstream dkim_value_without_b;
    dkim_value_without_b << "v=1; a=rsa-sha256; c=relaxed/simple; d=" << domain
                         << "; s=" << selector
                         << "; t=" << timestamp
                         << "; h=" << signed_header_list
                         << "; bh=" << body_hash
                         << "; b=";

    std::string signing_input;
    for (const auto& name : signed_headers) {
        auto it = headers.find(name);
        if (it == headers.end()) {
            error_out = "missing header for DKIM signing: " + name;
            return {};
        }
        signing_input += canonicalize_header_relaxed(name, it->second);
    }
    signing_input += canonicalize_header_relaxed("DKIM-Signature", dkim_value_without_b.str());

    std::string signature_b64;
    if (!sign_rsa_sha256_base64(signing_input, key_file, signature_b64, error_out)) {
        return {};
    }

    return "DKIM-Signature: " + dkim_value_without_b.str() + signature_b64 + "\r\n";
}

} // namespace

bool ensure_mail_raw_payload_loaded(mail& mail_data) {
    if (!mail_data.body.empty()) {
        return true;
    }
    if (mail_data.body_path.empty()) {
        return false;
    }

    mail_data.body = read_file_all(mail_data.body_path);
    return !mail_data.body.empty();
}

std::string build_outbound_message(const OutboxRecord& record,
                                   const mail* hot_mail,
                                   const std::string& header_from,
                                   const OutboundConfig& identity_config,
                                   bool* dkim_applied,
                                   std::string* dkim_error,
                                   std::string* message_id_out) {
    if (dkim_applied) {
        *dkim_applied = false;
    }
    if (dkim_error) {
        dkim_error->clear();
    }

    std::string raw_payload;
    if (hot_mail && !hot_mail->body.empty()) {
        raw_payload = hot_mail->body;
    } else {
        raw_payload = read_file_all(record.body_path);
    }
    if (!raw_payload.empty()) {
        std::string outbound_raw = raw_payload;

        std::string header_block;
        std::string body_block;
        if (split_rfc5322_message(raw_payload, header_block, body_block)) {
            auto raw_headers = parse_headers_relaxed_map(header_block);
            if (message_id_out) {
                auto it_mid = raw_headers.find("message-id");
                if (it_mid != raw_headers.end()) {
                    *message_id_out = it_mid->second;
                }
            }

            if (identity_config.dkim_enabled) {
                auto signed_headers = pick_present_headers_for_dkim(raw_headers);
                if (raw_headers.find("from") == raw_headers.end()) {
                    if (dkim_error) {
                        *dkim_error = "missing header for DKIM signing: from";
                    }
                } else {
                    std::string local_dkim_error;
                    const std::string dkim_header = build_dkim_header(
                        raw_headers,
                        normalize_body_simple(body_block),
                        identity_config,
                        local_dkim_error,
                        &signed_headers);
                    if (!dkim_header.empty()) {
                        if (dkim_applied) {
                            *dkim_applied = true;
                        }
                        outbound_raw = dkim_header + header_block + "\r\n\r\n" + body_block;
                    } else if (dkim_error) {
                        *dkim_error = std::move(local_dkim_error);
                    }
                }
            }
        }

        const std::string normalized_payload = normalize_for_smtp_data(outbound_raw);
        if (!normalized_payload.empty()) {
            return normalized_payload + ".\r\n";
        }
    }

    const std::string subject = "Outbound relay test";
    const std::string to_value = "<" + record.recipient + ">";
    const std::string from_value = "<" + header_from + ">";
    const std::string reply_to_value = "<" + record.sender + ">";
    const std::string date_value = format_rfc5322_date_utc();
    const std::string message_id = build_message_id(record);
    if (message_id_out) {
        *message_id_out = message_id;
    }
    const std::string mime_version = "1.0";
    const std::string content_type = "text/plain; charset=UTF-8";
    const std::string content_transfer_encoding = "8bit";
    const std::string body = "relayed by outbound client, outbox_id=" + std::to_string(record.id) + "\r\n";
    const std::string canonical_body = normalize_body_simple(body);

    std::unordered_map<std::string, std::string> dkim_headers;
    dkim_headers["from"] = from_value;
    dkim_headers["to"] = to_value;
    dkim_headers["subject"] = subject;
    dkim_headers["date"] = date_value;
    dkim_headers["message-id"] = message_id;
    dkim_headers["mime-version"] = mime_version;
    dkim_headers["content-type"] = content_type;
    dkim_headers["content-transfer-encoding"] = content_transfer_encoding;
    dkim_headers["reply-to"] = reply_to_value;

    std::ostringstream wire;
    if (identity_config.dkim_enabled) {
        std::string local_dkim_error;
        const std::string dkim_header = build_dkim_header(dkim_headers, canonical_body, identity_config, local_dkim_error);
        if (!dkim_header.empty()) {
            if (dkim_applied) {
                *dkim_applied = true;
            }
            wire << dkim_header;
        } else if (dkim_error) {
            *dkim_error = std::move(local_dkim_error);
        }
    }

    wire << "Subject: " << subject << "\r\n"
         << "From: " << from_value << "\r\n"
         << "To: " << to_value << "\r\n"
         << "Reply-To: " << reply_to_value << "\r\n"
         << "Date: " << date_value << "\r\n"
         << "Message-ID: " << message_id << "\r\n"
         << "MIME-Version: " << mime_version << "\r\n"
         << "Content-Type: " << content_type << "\r\n"
         << "Content-Transfer-Encoding: " << content_transfer_encoding << "\r\n"
         << "\r\n"
         << canonical_body
         << ".\r\n";

    return wire.str();
}

} // namespace outbound
} // namespace mail_system
