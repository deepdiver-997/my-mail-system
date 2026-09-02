#ifndef WEB_SERVER_H2_HPACK_H
#define WEB_SERVER_H2_HPACK_H
// ──────────────────────────────────────────────────────────────────
// HPACK 头压缩（RFC 7541）—— 独立 codec 组件，连接级有状态。
//
// 解码端：整数/字符串字面量 + 静态表 + 动态表 + 动态表容量更新 + Huffman。
//   HeaderDecoder 持动态表（跨 stream 共享，一个连接一个），逐帧解码 header block。
// 编码端（服务端发响应用，从简且安全）：:status 走静态表索引；其余用
//   "无索引字面量+原始字节(Huffman=0)"，不填动态表、不 Huffman —— 正确且无表状态。
// Huffman 表本身见 h2_huffman_table.h（自动生成，取自 RFC 7541 App B 权威码表）。
// ──────────────────────────────────────────────────────────────────
#include <cstdint>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace web_server {
namespace h2 {

// 一个已解码头
using Header = std::pair<std::string, std::string>;

// 静态表（RFC 7541 App A，61 项）
const Header& static_table_at(int index);   // 1..61
int  static_table_find(const std::string& name, const std::string& value); // 精确匹配 index，否则 0

// Huffman 解码（输入为 Huffman 编码字节，输出 ASCII）。失败返回 false。
bool huffman_decode(const std::string& in, std::string& out);

// 连接级有状态解码器：每个 H2 连接一个实例
class HeaderDecoder {
public:
    // 解码一段 header block → 按序的 (name,value) 列表；失败返回 false 并忽略已解析部分。
    bool decode(const std::string& block, std::vector<Header>& out);

    // 应用对端 SETTINGS_HEADER_TABLE_SIZE（动态表容量，超出现有容量会裁剪）
    bool set_header_table_size(uint32_t n);
    uint32_t header_table_size() const { return (uint32_t)capacity_; }
    size_t dynamic_bytes() const { return size_; }

private:
    void trim();

    struct Entry { std::string name; std::string value; };
    std::deque<Entry> dynamic_;   // [0] = 最新（索引 62）
    size_t size_ = 0;
    size_t capacity_ = 4096;      // RFC 默认 4096

    bool read_integer(const std::string&, size_t&, uint8_t prefix, uint32_t& out);
    bool read_string(const std::string&, size_t&, std::string& out);
    bool read_name(const std::string&, size_t&, uint32_t idx, std::string& out);
    const std::string* lookup(uint32_t index);   // 静态(1..61) 或动态(62..)名或值
};

// 编码响应头块：:status 走静态表索引；其余用无索引字面量(原始字节)
std::string encode_response_headers(const std::vector<Header>& headers);

} // namespace h2
} // namespace web_server

#endif // WEB_SERVER_H2_HPACK_H