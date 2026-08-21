# 交叉编译部署指南

## 架构

```
macOS ARM64 (开发机)              Linux x86_64 (服务器 <SERVER_IP>)
┌─────────────────────┐           ┌──────────────────────────┐
│ build.sh cross-x64  │  rsync   │ link.sh → imapsServer    │
│ → .o 文件 (ELF)     │ ──────→ │ → systemctl restart      │
│ artifacts/linux-*/  │  .o文件  │ 服务器本地 g++-13 链接    │
└─────────────────────┘           └──────────────────────────┘
```

- **编译**：macOS 上用 `x86_64-linux-gnu-g++` 交叉编译生成 `.o` 文件
- **链接**：`.o` 上传到服务器，用服务器本地 g++-13 链接（库 ABI 匹配）
- **原因**：macOS Homebrew 的 spdlog/fmt 版本与 Ubuntu 22.04 不同，交叉链接会失败

## 前置准备（仅首次）

### 1. 安装交叉编译器

```bash
brew install x86_64-linux-gnu-binutils
```

### 2. 同步服务器 sysroot 头文件

```bash
# 从服务器拉取 spdlog/fmt 头文件到本地 sysroot（家目录，重启不消失）
bash build.sh sync-sysroot
```

头文件保存在 `~/.protorelay/sysroot/usr/`，后续无需重复同步。仅当服务器更新 spdlog/fmt 时需要重新同步。

`build.sh` 在交叉编译时会**自动**：
- 检查 sysroot 头文件是否存在
- 设置 `SYSROOT_INCLUDE` cmake 变量
- 临时屏蔽 Homebrew 的 spdlog/fmt（避免 cmake 误找），configure 完成后自动恢复

## 完整流程

### 1. 编译

```bash
# 交叉编译（build.sh 自动处理所有细节）
EXTRA_CMAKE_ARGS="-DENABLE_S3_STORAGE=OFF" bash build.sh Release cross-x64 8
```

- `ENABLE_S3_STORAGE=OFF`：交叉编译器没有 libcurl，S3 模块需显式关闭（非 S3 场景均需此参数）
- `cross-x64` 自动使用 `x86_64-linux-gnu-g++` 并跳过链接（object-only）
- 产物在 `artifacts/linux-x86_64/Release/obj/`

### 2. 上传与链接（推荐用 deploy.sh，见下）

手工方式仅作理解用。核心注意点：**link.sh 必须显式给 `--obj-root`**，否则它会把
当前目录下所有 `.o`（包括历史遗留的 `build-obj/`）一并收集 → `multiple definition of main`。
过去靠"把 link.sh 拷到 /tmp 再跑"绕开，现在直接 `--obj-root` 指定即可。

```bash
ssh root@<SERVER_IP> "
cd /opt/smtpServer
# 入口 .o 在 test/server/ 下，注意这一层
bash link.sh obj/CMakeFiles/imapsServer_obj.dir/test/server/imaps_test.cpp.o \
  --obj-root obj -o imapsServer --compiler g++-13 \
  --exclude smtps_test --exclude mail_server
"
```

- `--obj-root obj`：只收集该目录下的 .o，不再碰 `build-obj/`
- `--exclude` 排除其它入口的 main（等价于旧的"删除 test entry point"，但不改文件）
- 用 `--compiler g++-13` 指定服务器编译器

### 3. 快速部署（smtpsServer）

```bash
# 同上，但入口文件是 smtps_test.cpp.o
./link.sh CMakeFiles/smtpsServer_obj.dir/test/smtps_test.cpp.o \
  -o /tmp/smtpsServer_new --compiler g++-13
```

## 一键部署

```bash
# 空跑：构建/上传/链接/校验全跑，但不替换线上二进制
DEPLOY_SERVER=root@<SERVER_IP> ./deploy.sh clean --dry-run

# 完整部署（先传后换 + 冒烟测试 + 失败自动回滚，详见 operations.md）
DEPLOY_SERVER=root@<SERVER_IP> ./deploy.sh clean
```

发布一律带 `clean`（避免 make 同秒时间戳跳过重编）。完整流程：同步 sysroot → 编译 →
rsync 增量上传到 `obj.new/` → 服务器链接成 `*.new` → 产物校验 → 自动备份 → 原子替换 →
重启 → 冒烟测试。

## 常见问题

### spdlog/fmt ABI 不匹配

**现象**：链接时报 `undefined reference to spdlog::details::log_msg::log_msg(..., fmt::v12::...)`

**原因**：macOS Homebrew 的 spdlog 使用 fmt v12，服务器 Ubuntu 22.04 使用 fmt v8（libfmt8）

**解决**：运行 `bash build.sh sync-sysroot` 同步服务器头文件。build.sh 交叉编译时自动使用 sysroot 中的头文件。

### Boost awaitable.hpp std::exchange 错误

**现象**：服务器直接编译时 `error: 'exchange' is not a member of 'std'`

**原因**：Boost 1.74 + g++-13 的兼容性 bug

**解决**：不要服务器直接编译。交叉编译的 Boost 版本无此问题。如果必须服务器编译，加 `-DBOOST_ASIO_DISABLE_AWAITABLE`

### multiple definition of main

**现象**：链接时报 `multiple definition of 'main'`

**原因**：link.sh 包含了所有目标的 `.o` 文件，包括 smtps_test.cpp.o 和 mail_server_combined.cpp.o

**解决**：用 `--obj-root` 限定对象根目录，并用 `--exclude` 排除不相关的 test entry point（不必删文件）。参见上文"上传与链接"。

### S3 storage undefined reference

**现象**：链接时报 `undefined reference to S3StorageProvider::S3StorageProvider(...)`

**原因**：交叉编译器没有 libcurl，S3 模块被跳过

**解决**：编译时加 `-DENABLE_S3_STORAGE=OFF`，代码中已有 `#ifdef ENABLE_S3_STORAGE` 保护

### 服务器编译卡死

**现象**：服务器上 `cmake --build` 内存耗尽

**原因**：服务器只有 2GB RAM，并行编译消耗大

**解决**：不要服务器编译，用交叉编译。交叉编译只生成 `.o`，链接在服务器上做（内存消耗小）

## 关键文件

| 文件 | 作用 |
|------|------|
| `build.sh` | 编译脚本，`cross-x64` 模式启用交叉编译（自动处理 Homebrew 屏蔽、sysroot） |
| `deploy.sh` | 一键部署脚本 |
| `artifacts/linux-x86_64/Release/obj/link.sh` | 服务器端链接脚本 |
| `~/.protorelay/sysroot/usr/` | 本地 sysroot 头文件缓存（spdlog/fmt） |
| `config/imapsConfig.json` | 服务器 IMAP 配置 |
| `/opt/smtpServer/` | 服务器部署目录 |
| `/etc/systemd/system/imapserver.service` | systemd 服务定义 |
