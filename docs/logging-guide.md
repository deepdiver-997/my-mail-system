# 日志系统配置说明

## 概述

项目使用 [spdlog](https://github.com/gabime/spdlog) 作为日志系统，支持模块化日志、多级别控制、文件滚动等功能。

## 编译时配置

### 启用/禁用调试日志

通过 CMake 编译选项控制：

```bash
# 构建时禁用所有调试日志（默认）
cmake -B build

# 构建时启用所有模块的调试日志
cmake -DENABLE_DEBUG_LOGS=ON -B build
```

### 模块化日志控制

在 `logger.h` 中定义了以下宏，编译时控制各模块的调试日志输出：

```cpp
// 在 logger.h 中定义的宏（默认值）
#define ENABLE_SERVER_DEBUG_LOG 0         // 服务器基础日志
#define ENABLE_NETWORK_DEBUG_LOG 0        // 网络连接日志
#define ENABLE_DATABASE_DEBUG_LOG 0        // 数据库连接池日志
#define ENABLE_DATABASE_QUERY_DEBUG_LOG 0  // 数据库查询详细日志（通常关闭）
#define ENABLE_SMTP_DEBUG_LOG 0           // SMTP 协议日志
#define ENABLE_SMTP_DETAIL_DEBUG_LOG 0     // SMTP 状态机详细日志（通常关闭）
#define ENABLE_SESSION_DEBUG_LOG 0         // 会话管理日志
#define ENABLE_THREAD_POOL_DEBUG_LOG 0      // 线程池日志
#define ENABLE_FILE_IO_DEBUG_LOG 0          // 文件 I/O 日志
#define ENABLE_AUTH_DEBUG_LOG 0             // 认证日志
```

**注意**：这些宏使用 `constexpr`，在编译时决定是否生成代码，**不会影响运行时性能**。

## 运行时配置

### 全局日志级别

在 `test/server/smtps_test.cpp` 的 `main()` 函数中初始化日志系统：

```cpp
Logger::get_instance().init(
    "logs/mail_system.log",           // 日志文件路径
    1024 * 1024 * 5,               // 单个文件最大大小 (5MB)
    3,                               // 保留文件数量
    spdlog::level::info              // 全局日志级别
);
```

### 日志级别

spdlog 支持以下日志级别（从低到高）：

| 级别 | 说明 | 使用场景 |
|--------|------|----------|
| `trace` | 最详细的跟踪信息 | 开发调试，跟踪代码执行路径 |
| `debug` | 调试信息 | 开发调试，查看变量值和状态 |
| `info` | 一般信息 | 正常运行信息，如连接建立、邮件发送等 |
| `warn` | 警告信息 | 潜在问题，如数据库连接慢、重试等 |
| `error` | 错误信息 | 运行时错误，需要关注但不影响系统运行 |
| `critical` | 严重错误 | 系统无法正常运行，需要立即处理 |

**建议**：
- 生产环境使用 `spdlog::level::info`
- 调试时使用 `spdlog::level::debug` 或 `spdlog::level::trace`
- 发布版本使用 `spdlog::level::warn` 或 `spdlog::level::error`

### 运行时动态调整日志级别

在代码中可以动态调整日志级别：

```cpp
// 设置全局日志级别
mail_system::set_log_level(spdlog::level::debug);

// 设置特定模块的日志级别
mail_system::set_module_log_level(mail_system::LogModule::DATABASE, spdlog::level::trace);
```

## 模块化日志使用

### 日志模块分类

| 模块 | 宏前缀 | 日志文件标识 | 使用场景 |
|--------|----------|----------------|----------|
| SERVER | `LOG_SERVER_*` | SERVER | 服务器启动、停止、配置加载 |
| NETWORK | `LOG_NETWORK_*` | NETWORK | TCP/SSL 连接接受、连接状态 |
| DATABASE | `LOG_DATABASE_*` | DATABASE | 数据库连接池管理、连接创建/销毁 |
| DB_QUERY | `LOG_DB_QUERY_*` | DB_QUERY | SQL 语句执行、prepared statement |
| SMTP | `LOG_SMTP_*` | SMTP | SMTP 协议命令处理（EHLO, MAIL FROM 等） |
| SMTP_DETAIL | `LOG_SMTP_DETAIL_*` | SMTP_DETAIL | 状态机状态转换、事件处理（通常关闭） |
| SESSION | `LOG_SESSION_*` | SESSION | 会话生命周期、错误处理 |
| THREAD_POOL | `LOG_THREAD_POOL_*` | THREAD_POOL | 线程池启动、停止、任务提交 |
| FILE_IO | `LOG_FILE_IO_*` | FILE_IO | 文件读写、保存邮件正文 |
| AUTH | `LOG_AUTH_*` | AUTH | 用户认证、权限验证 |

### 使用示例

```cpp
// SERVER 模块
LOG_SERVER_INFO("Server initialized with SSL: {}", enabled ? "enabled" : "disabled");
LOG_SERVER_ERROR("Failed to create MySQL pool: {}", e.what());

// NETWORK 模块
LOG_NETWORK_INFO("New SSL connection accepted");
LOG_NETWORK_ERROR("Error accepting SSL connection: {}", ec.message());

// DATABASE 模块（连接池级别）
LOG_DATABASE_DEBUG("Initializing pool");
LOG_DATABASE_WARN("Database pool is not initialized");

// DB_QUERY 模块（SQL 执行级别）
LOG_DB_QUERY_DEBUG("Executing SQL: {}", sql);
LOG_DB_QUERY_ERROR("MySQL query error: {}", mysql_error(m_mysql));

// SMTP 模块
LOG_SMTP_INFO("Mail received from: {}", sender);
LOG_SMTP_ERROR("Failed to parse mail address: {}", address);

// SMTP_DETAIL 模块（仅在需要调试状态机时启用）
LOG_SMTP_DETAIL_INFO("Current State: {}, Event: {}", state, event);
LOG_SMTP_DETAIL_WARN("No handler for state {} and event {}", state, event);
```

## 日志输出格式

### 默认格式

```
[2026-01-05 10:30:45.123] [info] [12345] [SERVER] Server initialized with SSL: enabled
```

格式说明：
- `[时间]` - 精确到毫秒的时间戳
- `[级别]` - 日志级别（trace/debug/info/warn/error/critical）
- `[线程ID]` - 输出日志的线程 ID
- `[模块名]` - 日志所属模块
- `消息` - 日志内容

### 格式化输出

使用 spdlog 的格式化语法：

```cpp
// 位置参数 {}
LOG_SERVER_INFO("Server started on port {}", port);

// 多个参数
LOG_SERVER_INFO("Server initialized with SSL: {}, TCP: {}", ssl_enabled, tcp_enabled);

// 十六进制输出
LOG_NETWORK_DEBUG("Connection ID: {:x}", connection_id);
```

## 日志文件管理

### 文件滚动

日志文件自动滚动：
- 单个文件最大 5MB（可在 `init()` 中修改）
- 保留最近 3 个文件
- 超出后自动删除旧文件

### 文件输出

- 同时输出到**终端**（彩色）和**文件**（纯文本）
- 终端输出：带颜色区分不同级别
- 文件输出：`logs/mail_system.log`

## 调试建议

### 场景 1：调试连接池问题

```bash
# 重新编译，启用数据库调试日志
cmake -DENABLE_DEBUG_LOGS=ON -B build
make

# 运行服务器
./build/smtpsServer

# 查看日志
tail -f logs/mail_system.log | grep DB_QUERY
```

### 场景 2：生产环境

```bash
# 正常编译（禁用详细日志）
cmake -B build
make

# 运行服务器
./build/smtpsServer

# 只查看错误日志
tail -f logs/mail_system.log | grep -E "(error|critical)"
```

### 场景 3：调试 SMTP 状态机

```bash
# 修改 logger.h，仅启用 SMTP_DETAIL
#define ENABLE_SMTP_DETAIL_DEBUG_LOG 1

# 重新编译
cmake -B build && make

# 查看状态机转换
tail -f logs/mail_system.log | grep SMTP_DETAIL
```

## 常用日志过滤命令

```bash
# 查看实时日志
tail -f logs/mail_system.log

# 过滤特定模块
tail -f logs/mail_system.log | grep -E "(SERVER|NETWORK)"

# 过滤错误和警告
tail -f logs/mail_system.log | grep -E "(error|warn)"

# 统计错误数量
grep -c "error" logs/mail_system.log

# 查看最近的错误
grep "error" logs/mail_system.log | tail -20
```

## 配置示例

### 开发环境

```cpp
// smtps_test.cpp
Logger::get_instance().init(
    "logs/mail_system.log",
    1024 * 1024 * 10,  // 10MB 单文件，方便开发调试
    10,                  // 保留 10 个文件
    spdlog::level::debug   // debug 级别
);
```

### 生产环境

```cpp
// smtps_test.cpp
Logger::get_instance().init(
    "logs/mail_system.log",
    1024 * 1024 * 5,   // 5MB 单文件
    3,                    // 保留 3 个文件
    spdlog::level::info    // info 级别
);
```

### 调试特定问题

```cpp
// smtps_test.cpp
Logger::get_instance().init(
    "logs/mail_system.log",
    1024 * 1024 * 5,
    3,
    spdlog::level::trace   // 最详细的级别
);

// 如果只关心数据库问题，运行时调整
mail_system::set_module_log_level(mail_system::LogModule::DATABASE, spdlog::level::trace);
mail_system::set_module_log_level(mail_system::LogModule::DB_QUERY, spdlog::level::trace);
```
