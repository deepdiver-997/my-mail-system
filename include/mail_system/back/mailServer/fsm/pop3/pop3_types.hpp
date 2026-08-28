#ifndef POP3_TYPES_HPP
#define POP3_TYPES_HPP

#include <boost/asio/steady_timer.hpp>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace mail_system {

// safe_stoull: imap_types.hpp 里的同名 inline 工具，独立放一份避免互相 include。
// RFC 1939 的 RETR/DELE 参数可能含非数字字符（旧客户端常带括号备注），用 std::stoull
// 会抛异常；这里返回 0 表示非法。范围 [1, SIZE_MAX)。
inline uint64_t safe_stoull(const std::string& s) {
    if (s.empty()) return 0;
    try { return std::stoull(s); } catch (...) { return 0; }
}

// ================================================================
// POP3 状态枚举 (RFC 1939 §3)
// ================================================================
enum class Pop3State {
    INIT = 0,            // 连接刚建立，未发送 +OK
    AUTHORIZATION = 1,   // 等 USER / PASS（连接刚 greet 完）
    TRANSACTION = 2,     // 鉴权完成，可访问 INBOX
    UPDATE = 3,          // QUIT 已收，apply 标记删除
    CLOSED = 4,          // 连接关闭
    COUNT = 5
};

// ================================================================
// POP3 事件枚举 —— 等价于命令名
// ================================================================
enum class Pop3Event {
    CONNECT = 0,
    CAPA = 1,
    USER = 2,
    PASS = 3,
    STAT = 4,
    LIST = 5,
    UIDL = 6,
    RETR = 7,
    DELE = 8,
    NOOP = 9,
    RSET = 10,
    QUIT = 11,
    ERROR = 12,
    TIMEOUT = 13,
    COUNT = 14
};

// ================================================================
// 单封邮件的 POP3 视图（IMAP MailboxMailInfo 的 POP3 子集）
// ================================================================
struct Pop3Message {
    uint64_t mail_id = 0;      // snowflake ID（也作为 UIDL）
    std::string body_path;     // 相对 storage key（stat 用）
    uint64_t size = 0;         // 字节数，PASS 时一次性查清缓存
};

// ================================================================
// POP3 上下文 —— 每会话
// ================================================================
struct Pop3Context {
    // 认证
    std::string username;                // USER 阶段暂存
    bool is_authenticated = false;
    uint64_t user_id = 0;
    uint64_t mailbox_id = 0;             // INBOX id（box_type=1）
    int shard_index = 0;                 // 由 shard router 分配

    // mailbox 快照（PASS 成功后填；后续 RETR/LIST 都基于此）
    std::vector<Pop3Message> messages;
    std::set<uint64_t> deleted;          // DELE 标记的 mail_id 集合

    // 生命周期
    bool dropped = false;                // QUIT 已发出，等 close
    std::string session_id;              // 全局唯一（snowflake 字符串）

    // 锁心跳（v2）：会话持有的 shared ticket。
    // 拿到锁后启动递归续约定时器，close() 时取消 → 破环。
    // heartbeat_handler 存递归回调本体（weak_tick 重新锁定用），
    // 两者都由 session 拥有，session 析构 → 定时器/回调一并释放，无泄漏。
    std::shared_ptr<boost::asio::steady_timer> heartbeat_timer;
    std::shared_ptr<std::function<void(const boost::system::error_code&)>> heartbeat_handler;

    // 命令 args（parse 后由 session 写入，handler 读）
    std::string last_command_args;

    void clear() {
        username.clear();
        is_authenticated = false;
        user_id = 0;
        mailbox_id = 0;
        shard_index = 0;
        messages.clear();
        deleted.clear();
        dropped = false;
        session_id.clear();
        heartbeat_timer.reset();
        heartbeat_handler.reset();
        last_command_args.clear();
    }
};

} // namespace mail_system

#endif // POP3_TYPES_HPP
