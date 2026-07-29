#ifndef MAIL_SYSTEM_FAST_FSM_BASE_H
#define MAIL_SYSTEM_FAST_FSM_BASE_H
// ──────────────────────────────────────────────────────────────────
// FastFsmBase — 编译期数组 O(1) 状态机基类
//
// 与 FsmBase (std::map) 的区别:
//   FsmBase:  std::map<pair<State,Event>, State>  → O(log N) 查找
//   FastFsmBase: State table_[S_COUNT][E_COUNT]   → O(1) 数组索引
//
// 要求: State 和 Event 枚举必须有 COUNT 哨兵值
//
// 数组大小 (编译期常量, 栈上分配):
//   SMTP: 12×11=132 项, 每项 ~48B (handler+state) → ~6KB
//   IMAP:  6×31=186 项 → ~9KB
//   出站: 12×13=156 项 → ~7KB
//
// handler 使用函数指针而非 std::function 可进一步优化 (见末尾 FastFsmPtr)
// ──────────────────────────────────────────────────────────────────

#include <array>
#include <functional>
#include <memory>
#include <type_traits>

namespace mail_system {

template <typename ConnectionType, typename State, typename Event>
class FastFsmBase {
    static_assert(std::is_enum_v<State>, "State must be an enum with COUNT sentinel");
    static_assert(std::is_enum_v<Event>, "Event must be an enum with COUNT sentinel");

public:
    static constexpr size_t kStateCount = static_cast<size_t>(State::COUNT);
    static constexpr size_t kEventCount = static_cast<size_t>(Event::COUNT);

    using Handler = std::function<void(
        std::shared_ptr<SessionBase<ConnectionType>>)>;

    virtual ~FastFsmBase() = default;

    // ── 注册接口 (初始化时调用) ──────────────────────────────────

    void add_transition(State from, Event on, State to) {
        size_t si = to_idx(from), ei = to_idx(on);
        transitions_[si][ei] = to;
    }

    void add_handler(State s, Event e, Handler h) {
        handlers_[to_idx(s)][to_idx(e)] = std::move(h);
    }

    // 添加 fallback handler: 某状态下所有未注册事件走同一个 handler
    void add_fallback_handler(State s, Handler h) {
        fallback_handlers_[to_idx(s)] = std::move(h);
    }

protected:
    // ── O(1) 分发 ──────────────────────────────────────────────

    bool dispatch(std::shared_ptr<SessionBase<ConnectionType>> session,
                  State current_state, Event event)
    {
        if (is_terminal_state(current_state)) {
            session->close();
            return true;
        }

        size_t si = to_idx(current_state);
        size_t ei = to_idx(event);

        // 1. 先查专用 handler
        auto& handler = handlers_[si][ei];
        if (handler) {
            pre_dispatch(current_state, event, session);
            handler(session);
            return true;
        }

        // 2. 回退到 fallback handler
        auto& fb = fallback_handlers_[si];
        if (fb) {
            pre_dispatch(current_state, event, session);
            fb(session);
            return true;
        }

        // 3. 无 handler → 无效转换
        on_invalid_transition(current_state, event, session);
        return true;
    }

    // ── 虚接口 (子类实现) ──────────────────────────────────────

    virtual bool is_terminal_state(State s) const = 0;
    virtual void on_invalid_transition(State s, Event e,
        std::shared_ptr<SessionBase<ConnectionType>> session) = 0;
    virtual void pre_dispatch(State, Event,
        std::shared_ptr<SessionBase<ConnectionType>>) {}

    // ── 工具 ───────────────────────────────────────────────────

    static constexpr size_t to_idx(State s) {
        return static_cast<size_t>(s);
    }
    static constexpr size_t to_idx(Event e) {
        return static_cast<size_t>(e);
    }

private:
    // 编译期定长数组 —— 完全在对象内分配，无堆间接访问
    std::array<std::array<Handler, kEventCount>, kStateCount> handlers_{};
    std::array<Handler, kStateCount> fallback_handlers_{};
    std::array<std::array<State, kEventCount>, kStateCount> transitions_{};
};

// ══════════════════════════════════════════════════════════════════
// FastFsmPtr — 函数指针版本, 消除 std::function 的 type-erasure 开销
//
// 用法: 子类声明成员函数指针, 构造时注册
//
//   template <typename C, typename S, typename E>
//   class FastFsmPtr {
//       using FnPtr = void (C::*)(std::shared_ptr<SessionBase<CT>>);
//       void add_handler_ptr(S s, E e, FnPtr fn);
//   };
//
// TODO: std::function 的 type-erasure 在 FSM 基准中占比 <3%,
// 当前收益不大，需要时再实现。
// ══════════════════════════════════════════════════════════════════

} // namespace mail_system

#endif // MAIL_SYSTEM_FAST_FSM_BASE_H
