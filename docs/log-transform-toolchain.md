# 日志压缩工具链 (Log Transform Toolchain)

## 概述

将 C++ 源码中的日志宏调用（如 `LOG_SERVER_INFO("port {}", 8080)`）在**构建时**替换为压缩形式
（如 `LOG_PURE(0xHASH, 8080, timestamp())`），运行时输出 `0xHASH|8080|1735426800123` 到专用
纯日志文件，后处理时通过增量映射表还原可读日志。

```
源码                       构建时变换               运行时输出
LOG_SMTP_INFO("HELO {}", d)  ──→  LOG_PURE(0xf69a..., d, ts)  ──→  0xf69aaaf2d05ba284|server|1735426800123
                                   │
                             pure_map.json                         log_restore.py
                             "0xf69a...": {                        │
                               "fmt": "HELO {}",             ──→  [2026-07-30 23:48:25][SMTP][INFO] HELO server
                               "level": "INFO",
                               "module": "SMTP"
                             }
```

## 设计要点

### Hash 稳定策略

```
hash = shake128("[LEVEL][MODULE]format_string") → 64-bit hex
```

**不含文件路径或行号**。同一格式串在项目任何位置得到相同 hash。开发者增删代码行不会导致 hash 变化，
映射表自然去重（本项目 504 个调用点 → 470 个唯一 hash）。

### 增量映射表

- 映射表**只追加不删除**：旧 hash 永久保留
- 同一行格式串改变时产生**新 hash**，旧 hash 不动
- 客户手里的任意版本映射表是公司最新表的子集 → 丢表后拿最新表照样还原

### 时间戳注入

每个 `LOG_PURE` 调用末尾自动追加 `mail_system::log_pure_timestamp_ms()`，
运行时输出 `hash|arg1|...|ts_ms`，还原时自动格式化为 `YYYY-MM-DD HH:MM:SS.mmm`。

## 工具链

```
tools/
├── log_transform.py   # 构建时变换 (Phase 1: gcc -E 分析, Phase 2: 正则替换)
├── log_restore.py     # 后处理还原 (支持 -f 实时 tail)
└── log_validate.py    # 端到端正确性验证
```

### log_transform.py — 构建时变换

**Phase 1: gcc -E 预处理器分析**

当编译时传入 `-DLOG_LOOK_UP`，`logger.h` 中所有 `LOG_*` 宏展开为带有 `\001` 分隔符的标记：

```c
(void)("LUK" "\001" "SMTP" "\001" "INFO" "\001" "HELO {}");
```

脚本运行 `gcc -E -DLOG_LOOK_UP` 对每个 `.cpp` 文件做预处理，通过 `#line` 指令追踪文件:行号，
提取 (文件, 行号, 模块, 级别, 格式串) 五元组。

**为什么用预处理器解析？**

- 正则无法正确处理多行宏、嵌套括号、原始字符串字面量
- `gcc -E` 是 C++ 宏语法的唯一权威解析器
- 通过 `#line` 指令精确获取跨头文件的源位置

**Phase 2: 源文件替换**

用正则匹配 + 括号计数找到宏调用的**完整跨度**（跨行正确），替换为 `LOG_PURE(hash, args, ts)`。

**输出**

| 产物 | 路径 | 说明 |
|------|------|------|
| 变换后源码 | `<out>/` | 全量项目拷贝，仅源文件被修改 |
| JSON 映射表 | `logs/pure_map.json` | 版本号 + entries |
| 嵌入头文件 | `include/generated/pure_map_data.h` | `constexpr std::string_view`，编译进 `.rodata` |

### log_restore.py — 日志还原

```bash
# 批量还原
python tools/log_restore.py logs/pure.log --map logs/pure_map.json

# 实时 tail
python tools/log_restore.py logs/pure.log -f
```

还原输出格式: `[时间] [级别][模块] 格式化消息`

### log_validate.py — 正确性验证

端到端流程：扫描所有日志调用 → 生成测试函数 → 构建原始版本捕获 baseline
→ 变换 → 构建变换版本 → 捕获 structured 输出 → 还原 → diff baseline。
零差异才算通过，可作为 CI 门禁。

## 集成方式

### 一键构建

```bash
./build.sh --pure-log Release
```

`--pure-log` 标志触发: 变换源码 → 从变换后的项目目录构建。

### 原位保护

`log_transform.py` 拒绝输出到项目目录自身，防止意外覆盖源码：

```
$ python tools/log_transform.py --out .
ERROR: Refusing to transform in-place.
  Output directory must differ from the project directory.
```

### 嵌入映射表

生成的头文件 `include/generated/pure_map_data.h` 可直接 `#include`：

```cpp
#include "generated/pure_map_data.h"

// 命令行 --dump-log-map 输出嵌入的映射表
if (arg == "--dump-log-map") {
    std::cout << mail_system::kPureMapData << std::endl;
}
```

## 数据流完整示意

```
开发源码                       构建产物                   运行时
LOG_INBOUND_INFO(               LOG_PURE(0x58ca...,       0x58cac9324bf53b85|
  "SPF pass for {}", d)          d, ts_fn())              "dom"|1785430541519
       │                              │                         │
       │    ┌──────────────────────────┘                         │
       │    │                                                    │
  [gcc -E 分析] ──→ pure_map.json ──→ include/generated/ ──→ log_restore.py
                     "0x58ca...": {     pure_map_data.h      │
                       "fmt": "...",    (嵌入 .rodata)       │
                       "level": "INFO",                 ──→ [2026-...][INFO][INBOUND]
                       "module": "INBOUND"                    SPF pass for dom
                     }
```

## 关于框架层集成的讨论

当前 `logger.h` 中混合了两层职责：

| 层 | 内容 | 归属 |
|----|------|------|
| 框架原语 | `log_pure_write`, `log_pure_timestamp_ms`, `LOG_PURE`, `Logger` 类 | 应属于 `framework/` |
| 应用宏 | `LOG_SERVER_*`, `LOG_DB_QUERY_*` 等 90 个宏 + `LOG_LOOK_UP` 标记 | 应属于 `mail_system/` |

理想的分离：

```
include/framework/log/          # 框架层 (可复用到任何项目)
├── logger.h                    # Logger 类, log()/set_log_level() API
├── log_pure.h                  # log_pure_write, log_pure_timestamp_ms, LOG_PURE 宏
└── log_lookup.h                # LOG_LOOK_UP 模式: __LOG_MARK 辅助宏

include/mail_system/back/common/ # 应用层 (项目特有)
└── app_log_macros.h             # 90 个 LOG_* 宏定义 + #ifdef LOG_LOOK_UP
```

这样框架提供原语，应用层只需注册模块名和级别名即可生成全套宏。
脚本也只处理应用层的 `app_log_macros.h`，框架层不受影响。

**是否现在重构**：功能已完整可用，框架分离属于架构改进，建议在下一个 release 前做。
