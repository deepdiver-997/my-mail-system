#ifndef MAIL_SYSTEM_ALGORITHM_LINE_FOLDER_H
#define MAIL_SYSTEM_ALGORITHM_LINE_FOLDER_H

#include <string>
#include <string_view>

namespace mail_system {
namespace algorithm {

// 入站正文行折叠器 —— 落盘前对超长行做 RFC 5322 折叠，给存储的原始报文
// 单行长度设上界（Postfix line_length_limit 同款防御）。
//
// 为什么在 SMTP 入站做：IMAP BODY[]/BODY[HEADER.FIELDS]、POP3 RETR 返回的都是
// 原始文件内容，DB 元数据的 subject 截断管不到这条路径；RFC 5322 的 998 字节
// 行上限对发件方只是 SHOULD，协议不强制，弱缓冲客户端解析超长行会直接断连
// （2026-09-05 test3 事故的同族风险：那次是 MySQL 列缓冲，这次预防的是
// 客户端/下游解析器）。
//
// 语义：
//   - 仅折叠超过 kMaxStorageLineBytes 的行，其余字节逐字直通 —— 正常邮件零改动。
//   - 折叠 = 在限内 UTF-8 字符边界处插入 CRLF + 单空格（RFC 5322 头折叠格式）。
//     按协议 unfold（删 CRLF 保留 WSP）后值里会多一个空格，仅出现在原本就
//     超长的行里；对 base64 正文解码无影响（解码器忽略空白）。
//     已知取舍：折叠点若落在 MIME encoded-word 中间可能破坏该头解码 ——
//     但不折叠的后果是整个邮件无法解析，两害取其轻。
//   - feed() 可跨块携带尾部半行（TCP 分块不保证行对齐）；数据结束必须 flush()。
class LineFolder {
public:
    static constexpr std::size_t kMaxStorageLineBytes = 2048;

    // 处理一块数据（可含多行、可含尾部半行），返回已折叠的完整行部分；
    // 尾部半行留存内部，等下一块或 flush()。
    std::string feed(const char* data, std::size_t size) {
        std::string out;
        out.reserve(size + size / 32);
        carry_.append(data, size);
        std::size_t pos = 0;
        while (true) {
            const std::size_t nl = carry_.find("\r\n", pos);
            if (nl == std::string::npos) break;
            append_folded(out, std::string_view(carry_).substr(pos, nl - pos));
            out += "\r\n";
            pos = nl + 2;
        }
        carry_.erase(0, pos);
        return out;
    }

    std::string feed(const std::string& s) { return feed(s.data(), s.size()); }

    // 数据结束：冲出携带的半行（同样按限折叠，无结尾 CRLF）。
    std::string flush() {
        if (carry_.empty()) return {};
        std::string out;
        append_folded(out, carry_);
        carry_.clear();
        return out;
    }

    void reset() { carry_.clear(); }

private:
    // 单行内容超限时逐段折叠；首段预算 kMax，后续段含前导折叠空格预算 kMax-1，
    // 保证每行（含续行的空格）内容都不超限。
    static void append_folded(std::string& out, std::string_view line) {
        std::size_t start = 0;
        bool first = true;
        while (line.size() - start > (first ? kMaxStorageLineBytes
                                            : kMaxStorageLineBytes - 1)) {
            const std::size_t budget =
                first ? kMaxStorageLineBytes : kMaxStorageLineBytes - 1;
            std::size_t cut = utf8_boundary(line, start + budget);
            if (cut == start) cut = start + budget;  // 病态字节序列，硬切
            out.append(line.substr(start, cut - start));
            out += "\r\n ";
            start = cut;
            first = false;
        }
        out.append(line.substr(start));
    }

    // 回退到 UTF-8 定界字节（line[cut] 不能是 10xxxxxx 续字节），最多回 3 字节。
    static std::size_t utf8_boundary(std::string_view line, std::size_t hard_pos) {
        std::size_t cut = hard_pos;
        while (cut > 0 &&
               (static_cast<unsigned char>(line[cut]) & 0xC0) == 0x80) {
            --cut;
        }
        return cut;
    }

    std::string carry_;   // 尾部半行（最后一段未见到 CRLF 的剩余字节）
};

} // namespace algorithm
} // namespace mail_system

#endif // MAIL_SYSTEM_ALGORITHM_LINE_FOLDER_H
