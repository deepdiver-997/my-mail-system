# 快速配置指南

## 日志系统配置

### 1. 编译配置

```bash
# 默认构建（生产环境推荐）
cmake -B build && cmake --build build -j4

# 调试构建（开发环境）
cmake -DENABLE_DEBUG_LOGS=ON -B build && make -j4
```

### 2. 运行配置

日志系统在 `test/server/smtps_test.cpp` 中自动初始化：

```cpp
Logger::get_instance().init(
    "logs/mail_system.log",    // 日志文件路径
    1024 * 1024 * 5,           // 单文件最大 5MB
    3,                               // 保留 3 个文件
    spdlog::level::info          // 默认日志级别
);
```

### 3. 运行时动态调整日志级别

在运行时可以通过 API 调整日志级别（需要重启）：

```cpp
// 在代码中调用
mail_system::set_log_level(spdlog::level::debug);      // 全局 debug
mail_system::set_log_level(spdlog::level::error);     // 只显示错误

// 或者只调整特定模块
mail_system::set_module_log_level(
    mail_system::LogModule::DATABASE,
    spdlog::level::trace);      // 数据库模块显示最详细日志
```

### 4. 查看日志

```bash
# 实时查看日志
tail -f logs/mail_system.log

# 只看错误
tail -f logs/mail_system.log | grep -E "(error|critical)"

# 只看网络日志
tail -f logs/mail_system.log | grep NETWORK

# 统计错误数量
grep -c "error" logs/mail_system.log
```

### 5. 模块快速参考

| 模块 | 宏 | 用途 |
|------|-----|------|
| SERVER | `LOG_SERVER_*` | 服务器启动、停止 |
| NETWORK | `LOG_NETWORK_*` | 网络连接 |
| DATABASE | `LOG_DATABASE_*` | 连接池管理 |
| DB_QUERY | `LOG_DB_QUERY_*` | SQL 执行 |
| SMTP | `LOG_SMTP_*` | 协议处理 |
| SESSION | `LOG_SESSION_*` | 会话管理 |

### 6. 常见场景

#### 场景 1：调试数据库连接问题

```bash
# 修改 logger.h，启用数据库调试
#define ENABLE_DATABASE_DEBUG_LOG 1

# 重新编译
cmake -B build && cmake --build build -j4

# 运行
./build/smtpsServer

# 查看数据库日志
tail -f logs/mail_system.log | grep DATABASE
```

#### 场景 2：生产环境运行

```bash
# 正常编译（调试日志全部关闭）
cmake -B build && cmake --build build -j4

# 运行
./build/smtpsServer

# 只监控错误
tail -f logs/mail_system.log | grep -E "(error|critical)"
```

#### 场景 3：只看 SQL 查询

```bash
# 修改 logger.h，只启用 SQL 查询调试
#define ENABLE_DATABASE_QUERY_DEBUG_LOG 1
#define ENABLE_DATABASE_DEBUG_LOG 0  # 禁用连接池调试
#define ENABLE_SMTP_DEBUG_LOG 0

# 重新编译并运行
cmake -B build && cmake --build build -j4
./build/smtpsServer

# 查看 SQL 执行日志
tail -f logs/mail_system.log | grep DB_QUERY
```

### 7. 修改单个模块日志级别

如果只想调试某个模块，修改 `logger.h` 中对应的宏：

```cpp
// 原值（所有调试关闭）
#define ENABLE_DATABASE_DEBUG_LOG 0
#define ENABLE_DATABASE_QUERY_DEBUG_LOG 0
#define ENABLE_SMTP_DEBUG_LOG 0
#define ENABLE_SMTP_DETAIL_DEBUG_LOG 0

// 修改为只启用数据库查询调试
#define ENABLE_DATABASE_DEBUG_LOG 0
#define ENABLE_DATABASE_QUERY_DEBUG_LOG 1  // 改为 1
#define ENABLE_SMTP_DEBUG_LOG 0
#define ENABLE_SMTP_DETAIL_DEBUG_LOG 0
```

### 8. 日志文件位置

- **默认**：`logs/mail_system.log`（相对于项目根目录）
- **修改**：编辑 `test/server/smtps_test.cpp` 中的 `init()` 调用
- **文件大小**：默认 5MB，修改 `init()` 的第二个参数
- **文件数量**：默认 3 个，修改 `init()` 的第三个参数

### 9. 注意事项

1. **编译时控制**：`ENABLE_*_DEBUG_LOG` 宏在编译时决定是否生成代码，运行时无法修改
2. **运行时控制**：`set_log_level()` 和 `set_module_log_level()` 可以在运行时调整
3. **优先级**：运行时设置优先级高于编译时宏设置
4. **性能**：禁用的调试日志不会产生任何运行时开销
5. **日志文件**：会自动创建 `logs/` 目录（如果不存在）

详细文档请参考：
- `docs/logging-guide.md` - 完整的日志系统说明
- `docs/prepared-statement-connection-pool-issue.md` - 数据库连接池问题分析
