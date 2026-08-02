#ifndef MAIL_SYSTEM_MAIL_CRYPTO_H
#define MAIL_SYSTEM_MAIL_CRYPTO_H

#include <openssl/evp.h>
#include <openssl/sha.h>
#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <vector>

namespace mail_system {
namespace outbound {

// ---------- string helpers ----------

inline std::string to_lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

inline std::string trim_ascii_ws(const std::string& input) {
    std::size_t begin = 0;
    while (begin < input.size() && (input[begin] == ' ' || input[begin] == '\t')) {
        ++begin;
    }
    std::size_t end = input.size();
    while (end > begin && (input[end - 1] == ' ' || input[end - 1] == '\t')) {
        --end;
    }
    return input.substr(begin, end - begin);
}

inline std::string collapse_ws(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    bool in_ws = false;
    for (unsigned char ch : input) {
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
            if (!in_ws) {
                out.push_back(' ');
                in_ws = true;
            }
            continue;
        }
        in_ws = false;
        out.push_back(static_cast<char>(ch));
    }
    return trim_ascii_ws(out);
}

inline std::vector<std::string> split_lines_lf(const std::string& text) {
    std::vector<std::string> lines;
    std::string current;
    current.reserve(text.size());
    for (char ch : text) {
        if (ch == '\n') {
            if (!current.empty() && current.back() == '\r') {
                current.pop_back();
            }
            lines.push_back(current);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty()) {
        if (current.back() == '\r') {
            current.pop_back();
        }
        lines.push_back(current);
    }
    return lines;
}

// ---------- DKIM canonicalization ----------

inline std::string canonicalize_header_relaxed(const std::string& field_name,
                                               const std::string& field_value) {
    return to_lower_copy(field_name) + ":" + collapse_ws(field_value) + "\r\n";
}

inline std::string normalize_body_simple(const std::string& body) {
    auto lines = split_lines_lf(body);
    while (!lines.empty() && lines.back().empty()) {
        lines.pop_back();
    }
    if (lines.empty()) {
        return "\r\n";
    }
    std::ostringstream oss;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        oss << lines[i] << "\r\n";
    }
    return oss.str();
}

// DKIM "relaxed" body canonicalization (RFC 6376 §3.4.4):
// - Ignores all trailing empty lines
// - Reduces runs of WSP within lines to a single SP
// - Removes trailing WSP from each line
inline std::string normalize_body_relaxed(const std::string& body) {
    auto lines = split_lines_lf(body);
    while (!lines.empty() && lines.back().empty())
        lines.pop_back();
    if (lines.empty())
        return "";
    std::ostringstream oss;
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string collapsed;
        bool in_sp = false;
        for (char ch : lines[i]) {
            if (ch == ' ' || ch == '\t') {
                in_sp = true;
            } else {
                if (in_sp) { collapsed += ' '; in_sp = false; }
                collapsed += ch;
            }
        }
        oss << collapsed << "\r\n";
    }
    return oss.str();
}

// ---------- crypto ----------

inline std::string base64_encode(const unsigned char* data, std::size_t size) {
    if (size == 0) {
        return {};
    }
    const int out_len = 4 * static_cast<int>((size + 2) / 3);
    std::string out(static_cast<std::size_t>(out_len), '\0');
    const int encoded = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(out.data()), data, static_cast<int>(size));
    if (encoded <= 0) {
        return {};
    }
    out.resize(static_cast<std::size_t>(encoded));
    return out;
}

inline std::string base64_decode(const std::string& encoded) {
    if (encoded.empty()) {
        return {};
    }
    const int in_len = static_cast<int>(encoded.size());
    // 输出长度：每 4 个输入字符产生 3 字节
    std::string out(static_cast<std::size_t>(3 * ((in_len + 3) / 4)), '\0');
    int decoded = EVP_DecodeBlock(
        reinterpret_cast<unsigned char*>(out.data()),
        reinterpret_cast<const unsigned char*>(encoded.data()), in_len);
    if (decoded <= 0) {
        return {};
    }
    // EVP_DecodeBlock 会在末尾追加填充字节，根据 '=' 数量剪掉
    int pad = 0;
    if (in_len > 0 && encoded[in_len - 1] == '=') ++pad;
    if (in_len > 1 && encoded[in_len - 2] == '=') ++pad;
    const int real_len = decoded - pad;
    if (real_len < 0) {
        return {};
    }
    out.resize(static_cast<std::size_t>(real_len));
    return out;
}

inline std::string sha256_base64(const std::string& data) {
    unsigned char digest[SHA256_DIGEST_LENGTH] = {0};
    if (!EVP_Digest(data.data(), data.size(), digest, nullptr, EVP_sha256(), nullptr)) {
        return {};
    }
    return base64_encode(digest, SHA256_DIGEST_LENGTH);
}

// 逐行流式计算 DKIM body canonicalization + SHA256，内存 O(行长)。
// 输出与 normalize_body_simple/relaxed + sha256_base64 严格等价。
//
// 输入是完整消息流（含 header）：自动跳过到首个空行分隔符，之后视为 body。
// trailing 空行剥离用"待发射的非空行 + 空行计数"实现：
//   - 空行先记数不发射（可能是 trailing）
//   - 下一个非空行到来时，才把上一 pending 行和其间空行一起发射（确认是中间行）
// canon: "simple" 或 "relaxed"
inline bool dkim_body_hash_stream(std::istream& in, const std::string& canon, std::string& out_base64) {
    SHA256_CTX ctx;
    SHA256_Init(&ctx);

    auto update = [&](const std::string& s) {
        SHA256_Update(&ctx, s.data(), s.size());
    };

    const bool is_relaxed = (canon == "relaxed");
    std::string pending;      // 最近一条非空行（canonical 后，不含 CRLF）
    bool have_pending = false;
    int pending_empties = 0;  // pending 行之后的空行数（可能 trailing）
    bool in_body = false;

    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (!in_body) {
            if (line.empty()) in_body = true;  // header/body 分隔符
            continue;
        }

        if (line.empty()) {
            pending_empties++;
            continue;
        }

        // canonical 当前行
        std::string cur;
        if (is_relaxed) {
            cur.reserve(line.size());
            bool in_sp = false;
            for (char ch : line) {
                if (ch == ' ' || ch == '\t') {
                    in_sp = true;
                } else {
                    if (in_sp) { cur += ' '; in_sp = false; }
                    cur += ch;
                }
            }
        } else {
            cur = line;
        }

        // 上一 pending 行已确定不是 trailing，发射它与其间的空行
        if (have_pending) {
            update(pending);
            update("\r\n");
            for (int i = 0; i < pending_empties; ++i) update("\r\n");
        } else {
            for (int i = 0; i < pending_empties; ++i) update("\r\n");
        }
        pending = std::move(cur);
        have_pending = true;
        pending_empties = 0;
    }

    if (have_pending) {
        update(pending);
        update("\r\n");
    } else if (!is_relaxed) {
        // simple：空 body 的 canonical 形式为单个 CRLF
        update("\r\n");
    }

    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256_Final(digest, &ctx);
    out_base64 = base64_encode(digest, SHA256_DIGEST_LENGTH);
    return true;
}

} // namespace outbound
} // namespace mail_system

#endif // MAIL_SYSTEM_MAIL_CRYPTO_H
