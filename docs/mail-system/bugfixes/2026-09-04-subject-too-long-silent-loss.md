# 超长主题静默丢信（1406 回滚）+ 连环加固

- 日期：2026-09-04 21:53 事故，当晚修复（a52795a）
- 影响：所有原始 Subject 头超 255 字符的入站邮件**静默丢失**——发件方已收
  250，收件人永远收不到，无退信无重试
- 生产表现：用户从华工邮箱（Coremail Webmail）转发邮件到 test3@scut.email，
  IMAP 登录后看不到新邮件

## 现象与定位

入站 SMTP 会话本身完全正常（DATA 接受、250 OK、正文落盘），但持久化阶段：

```
MySQL execute error: Data too long for column 'subject' at row 1 (errno: 1406)
Failed to insert mail metadata for mail ID ...
Transaction rolled back for mail_id=...
Processing failed ... cleanup_failed_mail 删除已落盘正文
```

IMAP 只读 DB——收件箱里根本没有这封邮件，客户端自然"看不到新邮件"。

## 根因（写入侧无兜底）

- `mails.subject` 列为 `varchar(255)`（utf8mb4，255 **字符**）；
- 代码把**原始 Subject 头**（含 MIME encoded-word、未经解码）原样入库：
  Q 编码使中文主题膨胀 3~4 倍，约五六十个汉字即爆列；
- `build_insert_mail` 只做 SQL 转义，无长度检查/截断；strict 模式下超长直接
  报 1406 → 整个持久化事务回滚 → `cleanup_failed_mail` 把已写盘的正文文件
  一并删除；
- `inbound_ack_mode: after_enqueue` 下 250 在入内存队列时就已发出，异步落库
  失败无法回滚 ACK → 发件方认为送达，实际丢失。

## 修复（a52795a）

1. **DB**：`mails.subject` 255 → 998（RFC 5322 单行上限量级），生产库已
   ALTER；迁移脚本 `config/sql/migration_mail_subject_len.sql`，仓库内
   create_tables 同步更新。
2. **代码**：新增 `db::sql::clip_subject_for_db` —— 按 escape 后字符预算
   截断（引号/反斜杠转义后翻倍计入预算），不切断 UTF-8 多字节序列；收口在
   SQL builder 层，覆盖 SMTP 入站批量/FSM DATA_END、IMAP APPEND 绑定参数、
   入站 subject 去重 WHERE（与库内截断值保持一致）。
3. **测试**：sql_queries_test 新增 ASCII/中文边界、转义膨胀预算、去重一致性
   等断言；生产端到端验证：1204 字符主题邮件截断到 998 正常入库入箱。

## 连环问题与后续加固

修复写入侧当晚，暴露出读取侧的同族缺陷——**IMAP 收件箱整体打不开**
（"单账号连不上"）：见 `../../bugfixes/2026-09-05-stmt-fetch-data-truncated.md`
（prepared stmt 结果缓冲 256 字节 + 不接受 DATA_TRUNCATED）。写入侧限长后
短时间即出现 >256 字节的 subject 行，两个缺陷形成接力。

预防层（710e887）：入站正文落盘前做超长行折叠（≤2048 字节/行，Postfix
line_length_limit 同款），使 IMAP BODY[HEADER.FIELDS]、POP3 RETR 等原始
内容路径不再可能见到怪物行——见
`include/mail_system/back/algorithm/line_folder.h` 头注释。

## 教训

1. **外部输入进 DB 前，写入侧必须有长度兜底**，不能依赖列定义报错兜底——
   strict 模式下报错=事务回滚=数据丢失，且发生在 ACK 之后无法通知发件方。
2. **防御要成对审视**：写入侧限长 + 读取侧容忍超长，缺一半都可能出事
   （本次两半各炸了一次）。
3. 转发/网关类场景（Coremail 等）的 encoded-word 主题是超长列的重灾区，
   测试应包含此类真实样本。
