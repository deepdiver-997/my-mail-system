# 数据库访问真异步化：MariaDB Connector/C 路线（设计）

> 状态：阶段 1、2、3 已实施（2026-08-29）。阶段 4（异步 checkout）待做。关联：
> [`mailbox-concurrency.md`](mailbox-concurrency.md)（single-flight 并发）、
> [`imap-server-design.md`](imap-server-design.md)（DB async CPS 现状）。

## 1. 背景与目标

IMAP/SMTP/POP3 的 FSM 已经把 DB 访问全部改成 CPS 链（`async_query`/`async_execute`
+ 回调续作，sq/se 同步桥已删），但底层 `async_*` 仍是**同步包装**——`cb(query(sql))`
内联执行。后果：

- **io 线程在 DB 查询期间完全卡死**。除 LOGIN/APPEND 走 worker 外，SELECT/FETCH/
  STORE/SEARCH/STATUS 等命令在 io 线程上直接跑同步 MySQL。
- **并发在途 DB 查询上限 = io 线程数**（配置 4）。每个 io 线程同时只能一条查询在途
  （阻塞式），且查询期间不服务任何其他连接。
- 单查询本地 ~0.5-3ms（含 fresh-stmt 的 prepare+execute 两趟往返），远程更高；
  io 线程的非 DB 工作是微秒级 → **DB 延迟是绝对主导**。

**目标**：

1. io 线程发出 DB 查询后立即回事件循环，不再死等 → 并发在途查询上限从 `#io 线程`
   提升到**连接池上限**（128），io 线程全程活跃。
2. 保留**参数化 prepared statement**（SQL 注入防护 + 类型安全），不退回 escape_string
   内联。
3. 连接保活继续做（不放养），但校验方式从 `SELECT 1` 换成 `mysql_ping()`。
4. 新增一套可切换的引擎实现，失败能切回。

## 2. 现状盘点（2026-08-29）

| 项 | 现状 |
|----|------|
| 客户端库 | libmysqlclient 9.5（mysql-client 包），**裸 C API** 直接调用（`mysql_init`/`mysql_real_connect`/`mysql_query`/`mysql_stmt_*`），无 C++ 包装；链接 `-lmysqlclient` 即执行者，非摆设 |
| `async_*` | `IDBConnection` 默认同步包装：`async_query(sql, cb) → cb(query(sql))` 内联 |
| 参数化 | `query(sql, params)` 每次调用**新建** prepared stmt（init→prepare→bind→execute→close），每查询两趟往返 |
| 保活/校验 | `MySQLPool::validate_connection()` = 每次 checkout 跑 `SELECT 1`（文本查询） |
| `SELECT 1` 丢参数？ | **当前不丢**：fresh stmt 在 SELECT 1 之后创建，两者无关。bugfix 文档（`docs/bugfixes/prepared-statement-connection-pool-issue.md`）说的是**旧缓存 stmt** 实现的坑 |
| 自动重连 | `MYSQL_OPT_RECONNECT` 被注释（未启用） |
| 事务 | **存在**：SMTP outbox 持久化 `persist_mail_transactional_async`（begin/commit/rollback，见 `persistent_queue.cpp:679`） |
| 引擎选择 | `config/db_config.json` 的 `"achieve"` 字符串；`server_base.cpp` 里 `achieve.find("mysql")==0` 分支建池 |
| io_context | `IOThreadPool` 每线程**一个独立 io_context**（配置 4 个），各线程各跑各的 `run()` |
| libmysqlclient 非阻塞 | 只有**文本协议**非阻塞（`mysql_real_query_nonblocking` 等），**无 prepared stmt 非阻塞** |

## 3. 决策（ADR）

### D1：换 MariaDB Connector/C 客户端库，不换服务器引擎

MariaDB Connector/C（libmariadb）是**客户端库**，与 MySQL/MariaDB **服务器**都兼容，
不是换引擎。头文件 `#include <mariadb/mysql.h>`，函数签名与 `<mysql/mysql.h>` 基本一致。
关键差异：它有**完整的非阻塞 API，包括 prepared statement**（`mysql_stmt_execute_start/cont`、
`mysql_stmt_fetch_start/cont` 等）——这正是 libmysqlclient 缺的。

**选择它的理由**：要"真异步 + 保留参数化 prepared statement"，libmysqlclient 只能二选一
（文本协议非阻塞 ⇒ 全 escape_string 内联）；MariaDB Connector/C 两个都要。代价是部署机
需要安装 libmariadb（见 §9 风险）。

### D2：保留连接保活，校验从 `SELECT 1` 改 `mysql_ping()`

**不放养连接**。放养（去掉 checkout 校验 + 失败重试）会让调用方处理
`CR_SERVER_GONE_ERROR` 重试，调用点变复杂且引入无法预测的抖动；而 pool 的
`idle_timeout=300s` 已远小于服务器 `wait_timeout`（默认 8h），死连接本来就是罕见事件，
校验只是兜底，保留它最稳。

`mysql_ping()` 发 COM_PING，**不跑查询、不产生结果集、不碰任何 prepared stmt 状态**，
是客户端库提供的正规保活手段。比 `SELECT 1` 更干净（不污染连接状态），是缓存 stmt
前提下的必要选择。

### D3：新增一套引擎实现，`achieve` 配置切换，可回滚

`IDBConnection` / `DBService` / `DBPool` 已是抽象边界。新增：
`MariaDBService`（工厂）+ `MariaDBConnection`（实现 `IDBConnection`）+
`MariaDBPoolFactory`，`server_base.cpp` 加 `achieve == "mariadb"` 分支。

回滚 = `config/db_config.json` 的 `"achieve"` 改回 `"mysql"`，旧引擎代码原样保留。
两条引擎并存，互不影响。

### D4：缓存 prepared statement（每连接 SQL→stmt）

现状每查询两趟往返（prepare + execute）。缓存后首次 prepare 一次，同 SQL 复用，
**一趟往返**。每连接独立缓存（prepared stmt 绑定连接），断连清空重建。

### D5：事务路径不依赖驱动自动重连

SMTP outbox 有真实事务。驱动 `MYSQL_OPT_RECONNECT` 会**静默丢失触发重连的那条语句**，
且重连后事务上下文不在——对事务是隐患。事务路径保持显式
`begin → ... → commit/rollback`，断连时由应用层检测 + 回滚/重来。单语句幂等操作
（IMAP 批量 STORE/COPY）也走显式重试而非驱动自动重连。

## 4. 引擎抽象与可切换性

```
config/db_config.json:  "achieve": "mariadb"   ← 或 "mysql"（回滚）
        │
server_base.cpp 分支:
  achieve == "mysql"        → MySQLPoolFactory + MySQLService   （现状，保留）
  achieve == "mariadb"      → MariaDBPoolFactory + MariaDBService（新增）
        │
DBPool（池 + 保活 + 连接生命周期）      IDBConnection（query/execute/async_*）
```

对外接口（`async_query`/`async_execute`/`async_begin_transaction`/...）不变，FSM
层零改动。池的配置项（initial/max/idle_timeout）语义不变。

## 5. MariaDB Connector/C 非阻塞设计

### 5.1 状态机（以 execute 为例）

```
mariadb_stmt_execute_start(stmt, bind, &result)
   │  返回 MYSQL_WAIT_READ / MYSQL_WAIT_WRITE / MYSQL_WAIT_OK / MYSQL_WAIT_ERROR / MYSQL_WAIT_TIMEOUT
   ▼
┌─ wait_for_socket(fd, event):   // fd = mariadb_get_socket(mysql)
│     io 线程调用  → io_context.async_wait(fd, readable/writable) → 事件回调里续
│     worker 调用  → 阻塞 poll(fd, event, timeout) → 就绪即续（worker 本就允许阻塞）
│
mariadb_stmt_execute_cont(stmt, 上次 wait 状态, &result)
   └── 循环直到 OK / ERROR
```

- **一套状态机代码，两种 wait 策略**：io 线程发起的查询 → 异步 `async_wait`（io 线程
  全程不阻塞）；worker 线程发起的（LOGIN bcrypt 链、APPEND 落盘链）→ 阻塞 poll
  （worker 本来就干阻塞活，可接受）。同一非阻塞状态机被两种 wait 复用。
- 结果拉取同状态机：`mysql_stmt_store_result_start/cont`、`mysql_stmt_fetch_start/cont`
  （含列截断、多结果集、错误路径）。
- **每连接单飞行**：MySQL 协议是请求-响应，一个连接同时只能一条在途。连接带
  忙/闲标记，异步等待 fd 期间该连接不可再取用。

### 5.2 连接池与 checkout

池的 checkout 目前是阻塞的（`m_cv.wait_for` 至多 connection_timeout）。本方案第一版
**保留同步 checkout**：池大（128）、io 线程少（4），io 线程因 checkout 阻塞的概率低。
若压测显示池耗尽成为新的 io 阻塞点，再做异步 checkout（回调式取连接 + 每连接在途队列），
列为阶段 4。

### 5.3 参数化与缓存

- 查询全走 prepared statement（保留注入防护）。缓存：每连接 `map<SQL, MYSQL_STMT*>`，
  首次 prepare，后续 bind + execute + `mysql_stmt_reset` 复用；断连/`mysql_ping` 检测到
  失效时清空重建。
- `mysql_ping()` 与缓存无冲突：ping 不碰 stmt 句柄。

## 6. 保活（`mysql_ping`）

```
validate_connection(conn):
  if (!conn->is_connected()) return false;
  return mysql_ping(conn->raw());   // COM_PING；可选配合 MYSQL_OPT_RECONNECT 透明重连
```

- 放在 checkout 时（同现在 `SELECT 1` 的位置），调用方零改动。
- 可选配合 `MYSQL_OPT_RECONNECT=1`：ping 发现死连接会自动重连；但**查询路径不依赖**
  它（见 D5）。
- 不引入每查询开销：ping 只在 checkout 时一次。

## 7. 事务支持

- 非阻塞 `BEGIN`/`COMMIT`/`ROLLBACK` 就是普通语句，状态机照跑（§5.1），
  `async_begin_transaction`/`async_commit`/`async_rollback` 语义不变。
- 断连检测：事务中连接死 → 语句返回 `CR_SERVER_GONE_ERROR` → 应用层
  `rollback` + 重来（SMTP outbox 现有逻辑已是显式事务，只需保证错误传播正确）。
- 不启用驱动自动重连（见 D5）。

## 8. 分阶段实施

| 阶段 | 内容 | 验收 |
|------|------|------|
| 1 | `MariaDBService`/`MariaDBConnection`/`MariaDBPoolFactory` 骨架，**同步**执行路径等价替换 libmysqlclient（`achieve=mariadb` 跑通全部查询/事务/SQL 脚本初始化） | 现有 ctest + e2e 全绿；切换后生产行为一致 |
| 2 | 非阻塞状态机 + io_context `async_wait` 集成（io 线程不再阻塞）；worker 路径走阻塞 poll | 压测：io 线程在查询期间有 idle；并发在途查询 > io 线程数 |
| 3 | prepared stmt 缓存（省 prepare 往返）+ 保活改 `mysql_ping()` | 往返次数降一半；保活回归 |

### 阶段 3 实施记录（2026-08-29）

- **prepared stmt 缓存**：`MariaDBConnection::m_stmtCache`（SQL→MYSQL_STMT*，cap 128）。
  `AsyncStmtOp` 缓存命中直接复用（跳过非阻塞 prepare），未命中新建 prepare 后入缓存。
  复用靠每次用后 `mysql_stmt_free_result`（结果已全量消费，省掉 `mysql_stmt_reset`
  那趟往返）；执行/读取出错 → 整体 `clear_stmt_cache()`（连接挂了缓存必然一起失效，
  宁可重 prepare）。断连时 `disconnect()` 统一关闭缓存 stmt。
- **保活改 `mysql_ping()`**：`IDBConnection::ping()`（默认 SELECT 1 兼容旧行为）+
  `MariaDBConnection::ping()` 覆写为 `mysql_ping`（COM_PING，不跑查询、不产生结果集、
  不碰 stmt 状态——缓存 stmt 后 SELECT 1 不再干净）。`MySQLPool::validate_connection`
  改走 `ping()`。
- **验证**：`SHOW GLOBAL STATUS` 实测 25,732 次 `Com_stmt_execute` 只有 **10 次
  `Com_stmt_prepare`**（distinct SQL 数）——每查询往返从 2 降到 1 ✓。localhost 吞吐
  提升小（~3%），因本地 prepare 本就快、轮次被 200 行结果 + storage 读主导；**对远程
  DB（部署目标）才是大头**。功能回归（LOGIN/SELECT/FETCH 经缓存 stmt 数据正确）、
  ctest 全绿、TSan 无 race。
| 4 | （可选）异步 checkout + 每连接在途队列 | 池耗尽不再阻塞 io |

每阶段独立可部署、可回滚（`achieve` 切回 `mysql`）。

### 阶段 2 实施记录（2026-08-29）

**已实现**（`mariadb_service.{h,cpp}` + `io_context_registry.{h,cpp}`）：

- `AsyncStmtOp` 非阻塞状态机：prepare → bind → execute → store_result → read，
  全部走 `mysql_stmt_*_start/cont`；`_start`/`_cont` 返回 0=完成（`*ret` 存阻塞版
  返回值）或 `MYSQL_WAIT_READ/WRITE/EXCEPT/TIMEOUT` 掩码。
- 两种 wait 策略（一套状态机）：`current_io_context()`（IOThreadPool 线程 thread_local
  注册）非空 → io 线程 → `io_context.async_wait`（posix::stream_descriptor assign/release
  托管 mariadb socket，首个触发者续作，其余忽略）；空（worker）→ 阻塞 poll，迭代不递归。
- 前置 `MYSQL_OPT_NONBLOCK`；缺非阻塞符号（老 libmariadb）→ `has_nonblocking()=false`
  → async_* 回退同步执行（旧行为）。
- `MariaDBConnection` 改 `enable_shared_from_this`；async op 持有连接 shared_ptr 保活。
- 单飞行守卫 `m_asyncInFlight`：FSM 链独占连接是结构性保证，flag 只做防御检测。

**压测结论（`achieve=mariadb`，localhost 200-mail 邮箱，imap_client SELECT+FETCH 1:200）**：

| 并发 | mariadb async (rounds/s) | mysql sync 同期 (rounds/s) |
|------|--------------------------|---------------------------|
| 1 | 586 | 609 |
| 4 | 1925 | 1816 |
| 8 | 1891 | 1910 |
| 16 | 1968 | 1975 |

两引擎在该 bench 上吞吐持平、同时封顶 ~1950 rps——**瓶颈是每轮非 DB 的 io 线程工作
（200 次 storage 读 + 200 行响应组装/写回），不是 DB 阻塞**；本地 DB socket 立即可读，
非阻塞查询也走内联完成（rc=0 无 wait），io 线程 CPU 并不因 async 降低。async 的收益在
远程/慢 DB 上才显现（socket 不就绪 → io 线程在 async_wait 期间真正让出）。SELECT-only
（缓存命中近零 DB）两引擎均 ~40k rps，mariadb 略低 5-13%（非阻塞状态机每次查询
init+prepare+execute+store+close 的开销，阶段 3 缓存 stmt 可消）。

**⚠ 实施中发现并修复的真 bug：async op 连接泄漏**。`done` 回调最初捕获了 `op`
自身（shared_ptr）→ `op`↔`done` 构成循环 → op（连同捕获了 ScopedConnection 的用户
回调）永不释放 → 每个 async 查询泄漏一条连接 → 池耗尽 → io 线程卡 connection_timeout
（5s）。压测复现：89 次 acquire 仅 1 次 release。修法：`done` 只捕获连接 + 用户 cb，
result/ok 以参数传入，绝不捕获 op。回归单测 `mariadb_async_test`：50 个 async 查询后
池 available 恢复基线（32→32）。**此 bug 同样影响 sync 之外的任何持链 CPS 异步化代码**。

**回归**：ctest 23/23（`-j4` 稳定）；TSan（imaps_fsm_test + mariadb_async_test）无 race。

## 9. 风险与回滚

- **部署机要装 libmariadb**（`mariadb-connector-c`）。构建期 CMake 加
  `find_library(MARIADB_CLIENT_LIB NAMES mariadb libmariadb)`；`mysql_service.cpp` 与
  `mariadb_service.cpp` 按 `achieve` 二选一编译/链接。CI、`deploy/`、Docker 镜像同步。
- **API 兼容性**：MariaDB Connector/C 与 libmysqlclient 函数签名基本一致，但
  `mariadb.h` 的 `MYSQL`/`MYSQL_STMT` 结构体定义不同 → 新引擎独立 `.cpp`，不共享
  结构体实现，避免 `#ifdef` 泥潭。
- **非阻塞状态机复杂度**：fetch 截断重拉、多结果集、错误/超时路径都要覆盖；这是
  阶段 2 的主要工作量。
- **每连接单飞行**：并发度受池大小（128）约束；多实例部署各自 128，够用。
- **回滚**：`achieve` 改回 `mysql` 即全部回退；两条引擎代码并存，不互删。

## 10. 验收

- 单元：mock DB 层（`test/unit/mock_db_pool.h`）基于 `IDBConnection` 抽象，**不动**；
  新增引擎的查询语义与旧引擎逐语句一致（`sql_queries_test` 等复用）。
- 集成：SQL 脚本初始化、事务（begin/commit/rollback）、批量 IN、大结果集截断。
- 并发：TSan + 压测；验证 io 线程在查询期间不再被 DB 阻塞（阶段 2 的验收点）。
- e2e：三协议（SMTP/IMAP/POP3）回归 + `test_imap_flow.py`/`test_pop3_flow.py`。
