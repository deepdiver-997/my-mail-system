#include "web_server/h2/h2_hpack.h"
#include "web_server/h2/h2_huffman_table.h"

namespace web_server {
namespace h2 {

// ════════════════════════════════════════════════════════════════
// 静态表（RFC 7541 Appendix A，61 项）
// ════════════════════════════════════════════════════════════════
namespace {
struct StaticEntry { const char* name; const char* value; };
// 值非空表示"名+值"整行；空串表示只有名字
const StaticEntry kStaticTable[61] = {
    {":authority", ""},                    // 1
    {":method", "GET"},                    // 2
    {":method", "POST"},                   // 3
    {":path", "/"},                        // 4
    {":path", "/index.html"},              // 5
    {":scheme", "http"},                   // 6
    {":scheme", "https"},                  // 7
    {":status", "200"},                    // 8
    {":status", "204"},                    // 9
    {":status", "206"},                    // 10
    {":status", "304"},                    // 11
    {":status", "400"},                    // 12
    {":status", "404"},                    // 13
    {":status", "500"},                    // 14
    {"accept-charset", ""},                // 15
    {"accept-encoding", "gzip, deflate"},  // 16
    {"accept-language", ""},               // 17
    {"accept-ranges", ""},                 // 18
    {"accept", ""},                        // 19
    {"access-control-allow-origin", ""},   // 20
    {"age", ""},                           // 21
    {"allow", ""},                         // 22
    {"authorization", ""},                 // 23
    {"cache-control", ""},                 // 24
    {"content-disposition", ""},           // 25
    {"content-encoding", ""},              // 26
    {"content-language", ""},              // 27
    {"content-length", ""},                // 28
    {"content-location", ""},              // 29
    {"content-range", ""},                 // 30
    {"content-type", ""},                  // 31
    {"cookie", ""},                        // 32
    {"date", ""},                          // 33
    {"etag", ""},                          // 34
    {"expect", ""},                        // 35
    {"expires", ""},                       // 36
    {"from", ""},                          // 37
    {"host", ""},                          // 38
    {"if-match", ""},                      // 39
    {"if-modified-since", ""},             // 40
    {"if-none-match", ""},                 // 41
    {"if-range", ""},                      // 42
    {"if-unmodified-since", ""},           // 43
    {"last-modified", ""},                 // 44
    {"link", ""},                          // 45
    {"location", ""},                      // 46
    {"max-forwards", ""},                  // 47
    {"proxy-authenticate", ""},            // 48
    {"proxy-authorization", ""},           // 49
    {"range", ""},                         // 50
    {"referer", ""},                       // 51
    {"refresh", ""},                       // 52
    {"retry-after", ""},                   // 53
    {"server", ""},                        // 54
    {"set-cookie", ""},                    // 55
    {"strict-transport-security", ""},     // 56
    {"transfer-encoding", ""},             // 57
    {"user-agent", ""},                    // 58
    {"vary", ""},                          // 59
    {"via", ""},                           // 60
    {"www-authenticate", ""},              // 61
};
} // namespace

const Header& static_table_at(int index) {
    static Header tmp;
    // 返回一个 (name,value) 视图；name-only 项 value 为空串
    static Header storage[61];
    static bool init = false;
    if (!init) {
        for (int i = 1; i <= 61; ++i) {
            storage[i - 1] = {kStaticTable[i - 1].name, kStaticTable[i - 1].value};
        }
        init = true;
    }
    if (index < 1 || index > 61) { tmp = {"", ""}; return tmp; }
    return storage[index - 1];
}
static int static_index_by_name(const std::string& name) {
    for (int i = 1; i <= 61; ++i)
        if (name == kStaticTable[i - 1].name) return i;
    return 0;
}

int static_table_find(const std::string& name, const std::string& value) {
    for (int i = 1; i <= 61; ++i)
        if (name == kStaticTable[i - 1].name && value == kStaticTable[i - 1].value)
            return i;
    return 0;
}

// ════════════════════════════════════════════════════════════════
// Huffman 解码（RFC 7541 App B，取自自动生成的 kHuffman 表）
// ════════════════════════════════════════════════════════════════
bool huffman_decode(const std::string& in, std::string& out) {
    // by_len[l] = kHuffman 中位长为 l 的条目下标（构建一次，magic static 线程安全）
    static const std::vector<std::vector<int>> by_len = [] {
        std::vector<std::vector<int>> v(31);
        for (int i = 0; i < kHuffmanCount; ++i)
            v[kHuffman[i].len].push_back(i);
        return v;
    }();

    out.clear(); out.reserve(in.size());
    uint32_t value = 0;
    int nbits = 0;
    for (unsigned char c : in) {
        for (int bit = 7; bit >= 0; --bit) {        // MSB-first
            value = (value << 1) | ((c >> bit) & 1);
            if (++nbits > 30) return false;          // 超过最长码也不匹配 = 非法
            if (nbits > (int)by_len.size() - 1) return false;
            bool matched = false;
            for (int idx : by_len[nbits]) {
                if (kHuffman[idx].bits == value) {
                    if (kHuffman[idx].sym == 256) return false;   // EOS 非法
                    out.push_back((char)kHuffman[idx].sym);
                    value = 0; nbits = 0; matched = true;
                    break;
                }
            }
            if (!matched && nbits > 30) return false;
        }
    }
    // 尾部 padding：解码后残留 ≤7 位，且必须是最长 EOS(11…1) 的前缀 → 全 1。
    // （RFC 7541 §5.2 用 EOS 的最高几位填字节边界，EOS=30 个 1，故 padding 是 1 位）
    if (nbits > 7) return false;
    if (nbits > 0) {
        uint32_t ones = (1u << nbits) - 1u;
        if (value != ones) return false;
    }
    return true;
}

// ════════════════════════════════════════════════════════════════
// HeaderDecoder：整数/字符串/静态表/动态表/容量更新/字面量
// ════════════════════════════════════════════════════════════════
bool HeaderDecoder::read_integer(const std::string& b, size_t& pos, uint8_t prefix, uint32_t& out) {
    if (pos >= b.size()) return false;
    uint8_t first = (uint8_t)b[pos++];
    uint32_t mask = (uint32_t)((1u << prefix) - 1);
    uint32_t value = (uint32_t)(first & mask);
    if (value < mask) { out = value; return true; }
    value = mask;
    uint32_t shift = 0;
    while (true) {
        if (pos >= b.size() || shift > 28) return false;
        uint8_t byte = (uint8_t)b[pos++];
        value += (uint32_t)(byte & 0x7f) << shift;
        shift += 7;
        if (!(byte & 0x80)) break;
    }
    out = value;
    return true;
}

bool HeaderDecoder::read_string(const std::string& b, size_t& pos, std::string& out_str) {
    if (pos >= b.size()) return false;
    uint8_t first = (uint8_t)b[pos];
    bool huff = (first & 0x80) != 0;
    uint32_t len;
    if (!read_integer(b, pos, 7, len)) return false;
    if (pos + len > b.size()) return false;
    std::string raw(b, pos, len);
    pos += len;
    if (huff) return huffman_decode(raw, out_str);
    out_str = std::move(raw);
    return true;
}

bool HeaderDecoder::set_header_table_size(uint32_t n) {
    capacity_ = n;
    trim();
    return true;
}

void HeaderDecoder::trim() {
    while (size_ > capacity_ && !dynamic_.empty()) {
        size_ -= 32 + dynamic_.back().name.size() + dynamic_.back().value.size();
        dynamic_.pop_back();
    }
}

// 索引 → 动态/静态条目，返回 name 或 value（用于名称查找）
const std::string* HeaderDecoder::lookup(uint32_t index) {
    if (index >= 1 && index <= 61) return &static_table_at((int)index).first;
    if (index >= 62) {
        size_t row = (size_t)(index - 62);
        if (row < dynamic_.size()) return &dynamic_[row].name;
    }
    return nullptr;
}

bool HeaderDecoder::read_name(const std::string& b, size_t& pos, uint32_t idx, std::string& out) {
    if (idx != 0) {
        const std::string* n = lookup(idx);
        if (!n) return false;
        out = *n;
        return true;
    }
    return read_string(b, pos, out);   // 新名字：字符串字面量
}

bool HeaderDecoder::decode(const std::string& block, std::vector<Header>& out) {
    size_t pos = 0;
    while (pos < block.size()) {
        uint8_t b = (uint8_t)block[pos];
        if (b & 0x80) {
            // 1xxxxxxx 索引头字段：索引静态/动态整行
            uint32_t idx;
            if (!read_integer(block, pos, 7, idx) || idx == 0) return false;
            if (idx >= 1 && idx <= 61) { const Header& e = static_table_at((int)idx); out.emplace_back(e); }
            else {
                size_t row = (size_t)(idx - 62);
                if (row >= dynamic_.size()) return false;
                out.emplace_back(dynamic_[row].name, dynamic_[row].value);
            }
        } else if (b & 0x40) {
            // 01xxxxxx 增量索引字面量：读名 + 值，加入动态表
            uint32_t idx;
            if (!read_integer(block, pos, 6, idx)) return false;
            std::string name, val;
            if (!read_name(block, pos, idx, name)) return false;
            if (!read_string(block, pos, val)) return false;
            out.emplace_back(std::move(name), std::move(val));
            // 加入动态表（列 62）
            uint32_t entry_size = 32 + (uint32_t)out.back().first.size() + (uint32_t)out.back().second.size();
            if (entry_size > capacity_) { dynamic_.clear(); size_ = 0; }   // 超大 → 清空
            else {
                dynamic_.push_front({out.back().first, out.back().second});
                size_ += entry_size;
                trim();
            }
        } else if (b & 0x20) {
            // 001xxxxx 动态表容量更新
            uint32_t sz;
            if (!read_integer(block, pos, 5, sz)) return false;
            set_header_table_size(sz);
        } else {
            // 0000xxxx / 0001xxxx 无索引字面量（不解码，不加入动态表）
            uint32_t idx;
            if (!read_integer(block, pos, 4, idx)) return false;
            std::string name, val;
            if (!read_name(block, pos, idx, name)) return false;
            if (!read_string(block, pos, val)) return false;
            out.emplace_back(std::move(name), std::move(val));
        }
    }
    return true;
}

// ════════════════════════════════════════════════════════════════
// 响应头编码：:status 走静态索引；其余无索引字面量（原始字节）
// ════════════════════════════════════════════════════════════════
static void write_integer_raw(std::string& out, uint32_t value, uint8_t prefix) {
    uint32_t mask = (1u << prefix) - 1;
    if (value < mask) { out += (char)value; return; }
    out += (char)mask;
    value -= mask;
    while (value >= 128) { out += (char)((value & 0x7f) | 0x80); value >>= 7; }
    out += (char)value;
}
static void write_string_raw(std::string& out, const std::string& s) {
    uint32_t len = (uint32_t)s.size(), mask = 0x7f;
    if (len < mask) out += (char)len;                       // huffman=0, low7=len
    else {
        out += (char)(0x80 | mask);
        uint32_t v = len - mask;
        while (v >= 128) { out += (char)((v & 0x7f) | 0x80); v >>= 7; }
        out += (char)v;
    }
    out += s;
}

std::string encode_response_headers(const std::vector<Header>& headers) {
    std::string out;
    for (const auto& h : headers) {
        int full = static_table_find(h.first, h.second);
        if (full > 0) { out += (char)(0x80 | full); continue; }   // 索引头字段
        int nidx = static_index_by_name(h.first);                  // 名在静态表 → 用名索引
        write_integer_raw(out, (uint32_t)nidx, 4);                 // 0000 + 名索引
        if (nidx == 0) write_string_raw(out, h.first);             // 新名字
        write_string_raw(out, h.second);                           // 值
    }
    return out;
}

} // namespace h2
} // namespace web_server