# 构建配置指南

## 概述

此 CMakeLists.txt 已优化为支持两种构建模式，每种模式有不同的编译参数和日志级别配置。

## 构建模式对比

| 特性 | Debug 模式 | Release 模式 |
|------|-----------|------------|
| **优化级别** | `-O0`（无优化） | `-O3`（高优化） |
| **本地优化** | ❌ | ✅ `-march=native` |
| **调试符号** | ✅ `-g` | ❌ |
| **帧指针** | ✅ `-fno-omit-frame-pointer` | ❌ |
| **NDEBUG** | ❌ | ✅ |
| **日志级别** | DEBUG（全部） | INFO（仅 INFO 和 WARNING） |
| **编译时间** | 快 | 较慢 |
| **运行时速度** | 慢 | 快 |
| **二进制大小** | 大 | 小 |

新增低内存兜底模式（`SafeRelease`）：
- 单线程构建（默认 `-j1`）
- 降低优化压力（`-O2`）
- 关闭调试日志编译开关
- 适用于 2核2G 等低内存服务器

## 快速开始

### 使用 build.sh 脚本

```bash
# Debug 构建（推荐用于开发/调试）
./build.sh Debug

# Release 构建（推荐用于测试/部署）
./build.sh Release

# SafeRelease 构建（低内存服务器推荐）
./build.sh SafeRelease

# SafeRelease 清理并重建
./build.sh SafeRelease clean

# 清理并重新构建
./build.sh Debug clean
./build.sh Release clean

# 指定并行度（可选，SafeRelease 默认就是 1）
./build.sh Release clean 1
BUILD_JOBS=1 ./build.sh Release
```

### 手动 CMake 命令

```bash
# Debug 模式
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Debug ..
make -j$(nproc)
cd ..

# Release 模式
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
cd ..
```

## 路径自动推导

CMakeLists.txt 现已支持自动查找依赖库，无需硬编码路径：

### MySQL
1. **优先级 1**: 通过 pkg-config 查找
2. **优先级 2**: 自动检测常见路径：
   - **macOS (Apple Silicon)**: `/opt/homebrew/opt/mysql-client`, `/opt/homebrew`
   - **macOS (Intel)**: `/usr/local/opt/mysql`, `/usr/local`
   - **Linux**: `/usr`, `/usr/local`, `/opt/mysql`

### OpenSSL
- 通过 CMake 的 `find_package` 自动查找
- macOS 回退路径：`/opt/homebrew/opt/openssl`, `/usr/local/opt/openssl`

## 日志配置

### Debug 模式（自动启用）
所有日志模块均启用：
- `ENABLE_SERVER_DEBUG_LOG=1`
- `ENABLE_NETWORK_DEBUG_LOG=1`
- `ENABLE_DATABASE_DEBUG_LOG=1`
- `ENABLE_DATABASE_QUERY_DEBUG_LOG=1`
- `ENABLE_SMTP_DEBUG_LOG=1`
- `ENABLE_SMTP_DETAIL_DEBUG_LOG=1`
- `ENABLE_SESSION_DEBUG_LOG=1`
- `ENABLE_THREAD_POOL_DEBUG_LOG=1`
- `ENABLE_FILE_IO_DEBUG_LOG=1`
- `ENABLE_AUTH_DEBUG_LOG=1`

### Release 模式（自动禁用）
所有 DEBUG 日志关闭，仅保留 INFO、WARNING、ERROR 级别。

## 手动覆盖日志设置

即使在 Release 模式下，也可以强制启用 DEBUG 日志：

```bash
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release -DENABLE_DEBUG_LOGS=ON ..
make -j$(nproc)
```

## 编译警告和诊断

启用的编译选项：
- `-Wall`: 所有常见警告
- `-Wextra`: 额外警告
- `-Wno-unused-parameter`: 抑制未使用参数警告（常见于模板代码）
- `-fdiagnostics-color=auto`: 彩色诊断输出（GCC/Clang）

## 性能优化 (Release)

Release 模式使用 `-march=native` 为当前 CPU 架构生成优化代码，通常提升 10-20% 性能。

## 输出目录结构

```
mail-system/v8/
├── build/
│   ├── CMakeFiles/     # 中间产物 (object files, etc.)
│   ├── smtpsServer     # SMTP 服务器可执行文件
│   ├── imapsServer     # IMAP 服务器可执行文件
│   └── ...
├── test/
│   ├── unit/           # C++ 单元测试
│   ├── bench/          # 性能基准
│   ├── e2e/            # Python 端到端测试
│   └── server/         # 服务器入口 main()
├── CMakeLists.txt
└── build.sh
```

## 故障排除

### MySQL 库未找到

如果 CMake 无法自动找到 MySQL，手动指定路径：

```bash
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release \
      -DMYSQL_INCLUDE_DIRS=/opt/homebrew/include \
      -DMYSQL_LIBRARIES=/opt/homebrew/lib/libmysqlclient.dylib \
      ..
make -j$(nproc)
```

### OpenSSL 库未找到（macOS）

```bash
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release \
      -DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl \
      ..
make -j$(nproc)
```

## 测试运行

### 冒烟测试（Python cl.py，支持 TLS/AUTH）
```bash
# 启动服务器
./build/smtpsServer -c config/smtpsConfig.json

# 另一个终端：Python 冒烟测试
cd test/scripts
python3 cl.py --mode mta-relay --messages 10 --concurrency 2 --verbose
```

### 性能压测（C++ smtp_client，推荐）
```bash
# 启动服务器
./build/smtpsServer -c config/smtpsConfig.json

# 全矩阵压测（详见 test/bench/bench-report.md）
./build/smtp_client --pipe --reuse --t 32 --msgs 50000   # 最大吞吐
./build/smtp_client --seq --reuse --t 16 --msgs 5000     # MTA 中继模拟
./build/smtp_client --pipe --t 4 --msgs 5000             # pipeline per-conn
./build/smtp_client --seq --t 8 --msgs 5000              # sequential per-conn
```

## 检查构建配置

每次构建时，CMake 会输出配置摘要：

```
-- Build type: Debug
-- ✓ SSL/TLS support ENABLED
-- 📝 Debug mode: All debug logs ENABLED
-- ✓ Found mysqlclient via pkg-config: ...
-- ✓ Found MySQL: ...
-- Build output: executables → .../test
-- Build output: intermediate → .../build/build
```
