# stmt 结果列超 256 字节缓冲中止整个结果集（IMAP 收件箱打不开）

- 日期：2026-09-05 发现并修复（d3a739c）
- 影响：收件箱内**任意一封**邮件的任意结果列超 256 字节 → 该邮箱的
  UID SEARCH/列表查询整体失败（`NO Server error`）→ 客户端表现即
  "账号连不上"，反复重连。生产实例：test3 收件箱一封 285 字节
  MIME 主题邮件让 test3 单账号故障一天；Sakura/test/qt 邮箱无长列
  邮件故全部正常，形成强烈的"单账号"假象。
- 生产：查询失败，无数据损坏。

## 现象

test3 账号 IMAP 登录成功但同步立即失败。协议级复现：

```
a2 SELECT INBOX            → OK（EXISTS/UIDNEXT 正常）
a3 UID SEARCH UID 1:*      → NO Server error     ← 全部列表查询失败
a4 UID FETCH 1:* (...)     → OK completed (empty)
```

服务端日志只有一句**空错误消息**：

```
[DB_QUERY] MySQL stmt fetch error:
```

## 根因

`MySQLConnection::query`（mysql_service.cpp，mariadb_service 同步/异步两处
同构）的结果拉取：

1. 绑定缓冲取 `fields[i].max_length`，而 prepared 语句未设
   `STMT_ATTR_UPDATE_MAX_LENGTH` 时该值**恒为 0** → 所有列用 256 字节兜底；
2. 列超缓冲时 `mysql_stmt_fetch()` 返回 **`MYSQL_DATA_TRUNCATED`(101)**——
   语义是"行可用、部分列被截断，可经 `mysql_stmt_fetch_column` 重取"，
   **不是错误**；旧 while 条件只接受 0 → 首个长列行直接退出循环，整个结果集
   按失败处理。循环体内写好的"检测截断 → 扩缓冲 → 重取该列"兜底逻辑
   成为死代码（作者误解了 API 返回值语义）。
3. `mysql_stmt_error()` 对 DATA_TRUNCATED 返回**空串**（非真错误），日志
   因此只剩光秃秃的一句，可诊断性为零。

## 时间线（为什么"昨天还能用，今天突然不行"）

- 09-04 22:40 修复超长主题入库（见 mail-system/bugfixes/2026-09-04）；
- 22:43 用户转发的邮件入库，subject 285 字节 > 256 → 从此刻起 test3 的
  每次列表查询必死。用户 22:44 前最后登录成功，之后"突然连不上"。
- 触发阈值之低（256 字节）意味着任何带正常长度主题/头列的邮件都可能踩中，
  与"超长主题"无必然关系——本缺陷是独立存在的读取侧防御缺失。

## 修复（d3a739c）

三处同构 fetch 循环统一接受 `MYSQL_DATA_TRUNCATED`，交由既有重取逻辑：

```cpp
while ((fetch_rc = mysql_stmt_fetch(stmt)) == 0 ||
       fetch_rc == MYSQL_DATA_TRUNCATED) { ... }
```

错误分支同时带上 rc 数值（2299575）：`stmt fetch error: rc={} msg='{}'`。

## 测试

`mariadb_async_test` 新增 900 字节列回归用例（async + MySQL 同步双路径，
断言长列完整取回）；修复前实测复现失败。全套 24/24。

## 教训

1. **兜底代码不可达 = 没有兜底**。写防御逻辑前先确认触发条件真实可达
   （本次：TRUNCATED 时 while 条件已经把它挡在外面）。
2. **错误日志必须带数值型状态码**。`mysql_stmt_error`/`strerror` 类辅助
   函数对非标准状态可能返回空串，`rc=101` 一眼可辨，空串等于零信息。
3. 单账号故障先查数据差异（该账号邮箱里有什么别的账号没有的行），
   再查网络/客户端——本次"只有 test3 不行"直接指向了数据触发的查询失败。
