// IntrusionDetector 单元测试：会话记录、私网过滤、封禁阈值、LRU 上限、持久化。
//
// 生产里它跑在 server_base 的会话结束路径上，此前无直接测试。
// 注意两点测试约束：
//   1) LRU 驱逐按 last_seen（秒级墙钟）比较，同秒插入的记录 last_seen 相同，
//      驱逐对象不可预测 —— 只断言记录数有界，不断言具体被驱逐的 IP。
//   2) 懒刷盘的触发条件是「记录数 ≥ 脏阈值 且 (首刷或已过刷盘间隔)」，
//      测试把 interval 设 0 保证第二次记录即触发。
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <string>

#include "framework/intrusion_detector.h"

namespace {

using mail_system::IntrusionDetector;

int g_pass = 0, g_fail = 0;
void expect_true(bool c, const char* what) {
    if (c) { g_pass++; }
    else { g_fail++; std::printf("  FAIL %s\n", what); }
}

std::filesystem::path tmp_dir() {
    return std::filesystem::temp_directory_path() / "protorelay_intrusion_test";
}

} // namespace

int main() {
    std::printf("intrusion_detector_test\n");
    auto dir = tmp_dir();
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    // 1. 未 enable：record_session 不记录
    {
        IntrusionDetector d;
        d.record_session("8.8.8.8", false);
        expect_true(d.size() == 0, "disabled detector records nothing");
        expect_true(!d.is_enabled(), "disabled by default");
    }

    // 2. 空 IP：即便 enable 也不记录
    {
        IntrusionDetector d;
        d.set_enabled(true);
        d.record_session("", false);
        expect_true(d.size() == 0, "empty ip ignored");
    }

    // 3. 私网 IP 全部跳过，公网 IP 正常记录
    {
        IntrusionDetector d;
        d.set_enabled(true);
        const char* privates[] = {"127.0.0.1", "::1", "0.0.0.0",
                                  "10.1.2.3", "192.168.0.1",
                                  "172.16.0.1", "172.31.255.255"};
        for (auto ip : privates) d.record_session(ip, false);
        expect_true(d.size() == 0, "private/loopback ips skipped");

        d.record_session("172.32.0.1", false);   // 172.32 不在 16-31 区间
        d.record_session("172.15.0.1", false);   // 172.15 不在 16-31 区间
        d.record_session("8.8.8.8", false);
        expect_true(d.size() == 3, "public ips recorded");
    }

    // 4. 计数：total_connections 累加，failed_count 仅未认证时累加
    {
        IntrusionDetector d;
        d.set_enabled(true);
        d.record_session("8.8.8.8", false);
        d.record_session("8.8.8.8", false);
        d.record_session("8.8.8.8", true);
        auto r = d.query("8.8.8.8");
        expect_true(r.total_connections == 3, "total_connections increments each session");
        expect_true(r.failed_count == 2, "failed_count only counts unauthenticated");
        expect_true(r.first_seen != 0 && r.last_seen != 0, "timestamps populated");
    }

    // 5. 封禁阈值：默认 0 永不放行；设阈值后达标放行
    {
        IntrusionDetector d;
        d.set_enabled(true);
        for (int i = 0; i < 5; i++) d.record_session("1.1.1.1", false);
        expect_true(!d.is_banned("1.1.1.1"), "ban_threshold=0 never bans");
        expect_true(!d.is_banned("8.8.8.8"), "unknown ip never banned");

        d.set_ban_threshold(3);
        expect_true(d.is_banned("1.1.1.1"), "failed_count>=threshold bans");
        expect_true(!d.is_banned("9.9.9.9"), "sub-threshold ip not banned");

        IntrusionDetector d2;
        d2.set_enabled(true);
        d2.set_ban_threshold(3);
        for (int i = 0; i < 2; i++) d2.record_session("2.2.2.2", false);
        expect_true(!d2.is_banned("2.2.2.2"), "below threshold not banned");
    }

    // 6. LRU 上限：超 max_records 后记录数有界
    {
        IntrusionDetector d;
        d.set_enabled(true);
        d.set_max_records(5);
        for (int i = 0; i < 50; i++)
            d.record_session("ip-" + std::to_string(i) + ".example", false);
        expect_true(d.size() == 5, "records capped at max_records");
        expect_true(d.snapshot().size() == 5, "snapshot reflects cap");
    }

    // 7. persist / restore 往返一致
    {
        auto file = (dir / "detector.json").string();
        {
            IntrusionDetector d(file);
            d.set_enabled(true);
            d.record_session("8.8.8.8", false);
            d.record_session("1.1.1.1", true);
            expect_true(d.persist(), "persist returns true");
        }
        IntrusionDetector d2(file);
        expect_true(d2.restore(), "restore returns true");
        expect_true(d2.size() == 2, "restored record count");
        expect_true(d2.query("8.8.8.8").failed_count == 1, "restored failed_count");
        expect_true(d2.query("1.1.1.1").failed_count == 0, "restored authenticated record");
    }

    // 8. 懒刷盘：脏阈值 2 + 间隔 0 → 第二次记录即落盘
    {
        auto file = (dir / "lazy.json").string();
        {
            IntrusionDetector d(file);
            d.set_enabled(true);
            d.set_persist_dirty_threshold(2);
            d.set_persist_interval(0);
            d.record_session("8.8.8.8", false);
            expect_true(std::filesystem::exists(file) == false,
                        "no persist before dirty threshold");
            d.record_session("1.1.1.1", false);
            expect_true(std::filesystem::exists(file),
                        "lazy persist fires at dirty threshold");
        }
    }

    // 9. 空 data_file：persist/restore 返回 false
    {
        IntrusionDetector d("");
        d.set_enabled(true);
        d.record_session("8.8.8.8", false);
        expect_true(!d.persist(), "empty data_file persist -> false");
        IntrusionDetector d2("");
        expect_true(!d2.restore(), "empty data_file restore -> false");
    }

    // 10. setter 生效
    {
        IntrusionDetector d;
        d.set_max_records(7);
        d.set_ban_threshold(4);
        d.set_persist_interval(30);
        d.set_persist_dirty_threshold(10);
        d.set_enabled(true);
        for (int i = 0; i < 9; i++) d.record_session("x" + std::to_string(i), false);
        expect_true(d.size() == 7, "max_records setter caps size");
    }

    std::filesystem::remove_all(dir);
    std::printf("  pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
