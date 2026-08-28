// MariaDB 非阻塞 async 的回归测试（阶段 2）。
//
// 剧情：连接池的 available 连接数在跑完一批 async 查询后必须回到基线。
// 这直接守卫 2026-08-29 修掉的一个真 bug：AsyncStmtOp::done 若捕获 op 自身，
// op↔done 构成 shared_ptr 循环 → op（连同捕获了 ScopedConnection 的用户回调）
// 永远不释放 → 每个 async 查询泄漏一条连接 → 池耗尽 → io 线程卡 5s
// connection_timeout。压测复现：89 次 acquire 只有 1 次 release。
//
// 依赖真实 DB（config/db_config.json 指向的 MySQL/MariaDB 服务 + mail 库）：
// 文件不存在或连不上 → 静默 SKIP（返回 0），不影响 CI。
// 本测试跑在主线程（无 io_context 注册）→ async 走阻塞 poll 路径，回调同步触发。

#include "mail_system/back/db/mariadb_pool.h"
#include "mail_system/back/db/mariadb_service.h"
#include "mail_system/back/db/db_pool.h"
#include <iostream>

using namespace mail_system;

int main() {
    DBPoolConfig cfg;
    bool loaded = false;
    // 测试从 build/ 目录跑（ctest WorkingDirectory），config 在项目根
    for (const char* p : {"config/db_config.json", "../config/db_config.json"}) {
        if (cfg.loadFromJson(p)) { loaded = true; break; }
    }
    if (!loaded) {
        std::cout << "SKIP: db_config.json not found\n";
        return 0;
    }

    auto pool = MariaDBPoolFactory::get_instance().create_pool(
        cfg, std::make_shared<MariaDBService>());
    if (!pool) { std::cout << "SKIP: pool creation failed\n"; return 0; }

    // 探活：连不上真实 DB 就 skip（本地 CI 无 DB 也能过）
    {
        auto probe = pool->acquire_connection();
        if (!probe.is_valid()) {
            std::cout << "SKIP: database unavailable (" << pool->get_available_connections()
                      << " avail)\n";
            return 0;
        }
    }

    const size_t initial_avail = pool->get_available_connections();
    int completed = 0;
    constexpr int kQueries = 50;

    for (int i = 0; i < kQueries; ++i) {
        auto sc = pool->acquire_connection();
        if (!sc.is_valid()) break;
        bool done = false;
        sc->async_query("SELECT 1", [&](std::shared_ptr<IDBResult> r) { done = (r != nullptr); });
        // 主线程 → 阻塞 poll 路径，回调同步触发；失败即代表 async 链断了
        if (done) ++completed;
        // sc 出作用域 → release_connection。旧代码（done 捕获 op 的循环）这里不归还，
        // available 持续下降，最后一条断言触发 FAIL。
    }

    const size_t after_avail = pool->get_available_connections();

    if (completed != kQueries) {
        std::cout << "FAIL: only " << completed << "/" << kQueries
                  << " async queries completed\n";
        return 1;
    }
    if (after_avail < initial_avail) {
        std::cout << "FAIL: connection leak — available dropped from " << initial_avail
                  << " to " << after_avail << " after " << kQueries << " async queries\n";
        return 1;
    }

    std::cout << "PASS: " << completed << " async queries, pool available "
              << initial_avail << " -> " << after_avail << " (no leak)\n";
    return 0;
}
