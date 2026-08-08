# 2026-08-02 SMTP/IMAP MIME 与协议修复记录

## 背景

网易邮件大师(IMAP 客户端)连接 ProtoRelay 时出现一系列问题:QQ/Anthropic/Claude/飞书邮件正文不显示、已读标记不持久、客户端反复全量重拉。逐层排查后确认多为**服务端 MIME 解析与 IMAP 协议实现不符合 RFC**。

---

## 问题清单

### 1. BODYSTRUCTURE multipart 格式错误(RFC 3501 §7.4.2)

**症状:** 客户端显示邮件为空。

**根因:** [traditional_imaps_fsm.tpp](../../include/mail_system/back/mailServer/fsm/imaps/traditional_imaps_fsm.tpp) `build_bodystructure_tree` 把 multipart 输出成单 part 布局:
```
("multipart" "alternative" ("BOUNDARY" "...") NIL NIL NIL (sub1)(sub2))
```
RFC 要求**子 part 在前、subtype 在后**:
```
((sub1)(sub2) "alternative" ("BOUNDARY" "...") NIL NIL NIL)
```

**修复:** multipart 分支改为先递归子 part,再输出 subtype + boundary。

### 2. BODY[n] 返回解码内容(RFC 3501 §6.4.5)

**症状:** 客户端反复重试 BODY.PEEK[n]。

**根因:** 曾错误地做 base64/quoted-printable 解码返回。RFC 3501 规定 `BODY[<section>]` 返回**原始编码内容**,由客户端根据 BODYSTRUCTURE 的 body-fld-enc 自行解码。

**修复:** `extract_part_content` 只跳过该 part 的 MIME header,返回原始编码字节。

### 3. 非 multipart 根 part length 只到 header

**症状:** 单 part 邮件的 BODY[1] 返回空,客户端看不到正文。

**根因:** `parse_mime_tree` 对非 multipart 根 part 只把 `length` 设为 header 长度,`extract_part_content` 只取到 header 段。

**修复:** 根 part `length = raw.size() - pos`(整封邮件);`extract_part_content` 兼容旧 sidecar(length 不足时正文延伸到消息末尾)。

### 4. charset 不带引号不解析

**症状:** BODYSTRUCTURE 的 CHARSET 为 NIL。

**根因:** `parse_mime_tree` 只处理 `charset="..."`,不处理 `charset=us-ascii`。

**修复:** 支持带引号与不带引号两种写法。

### 5. boundary 提取两个 bug

**症状:** multipart 邮件解析不出子 part(飞书/某些 MTA 邮件正文不显示)。

**根因 A:** 不带引号且 boundary 在 header 末尾时,`find_first_of(" \t\r\n", os)` 返回 npos → boundary 丢失。
**根因 B:** 不带引号时 `find('"', obp)` 向后搜引号,误匹配后续 `To: "test3"` 的引号,把 boundary 提取成 `"test3"`。

**修复:** 直接取 `boundary=` 之后的位置(`obp+9`),只检查该位置是否带引号,不再向后搜。

### 6. \Seen 持久化两个 bug

**症状:** 客户端标记已读后邮件仍是未读,反复重拉。

**根因 A:** [mysql_service.cpp](../../src/mail_system/back/db/mysql_service.cpp) prepared statement 与连接池 `SELECT 1` 验证不兼容,绑定参数丢失(见 [prepared-statement-connection-pool-issue.md](prepared-statement-connection-pool-issue.md))。`update_mail_seen` 改用直接 SQL + escape_string。
**根因 B:** STORE 的 flag 解析大小写敏感,客户端发 `\SEEN`(大写)不匹配 `\Seen` → flag_seen=false → **update_mail_seen 从未被调用**。改成大小写不敏感,并修正 `-FLAGS.SILENT` 被 `find("FLAGS")` 先匹配丢负号的顺序 bug。

### 7. SMTP 25 端口 530 拒绝外部投递

**症状:** OpenAI/GPT 验证码邮件收不到。

**根因:** [traditional_smtps_fsm.tpp](../../include/mail_system/back/mailServer/fsm/smtps/traditional_smtps_fsm.tpp) 用 listener 的 `auth_policy`(AUTO)决定 `require_auth = !is_trusted_server`,而该值从未被置真 → 所有外部发件人被 530。配置里 `inbound_auth_policy: off` 未生效。

**修复:** AUTO 端口的认证要求改由 `cfg->inbound_auth_policy` 决定;465/587(ON)仍强制认证。

### 8. RECENT 公式错误 + 缓存不失效

**症状:** 客户端每隔数秒完整登录重拉。

**根因 A:** SELECT 里 `RECENT = count - unseen`(已读数量),只要有已读邮件就恒 > 0,客户端永远认为有新邮件。
**根因 B:** 邮箱统计 LRU 缓存(TTL 5s)在 STORE 后未失效,SELECT 返回旧的 UNSEEN。

**修复:** RECENT 报告 0(未跟踪 \Recent);STORE 后 `m_mailboxStatsCache->invalidate()`;后台 stale-while-revalidate 按 key 去重防雪崩。

### 9. SEARCH / FETCH 的 UID 范围过滤缺失

**症状:** 客户端"最后同步 UID"永远无法推进,每次轮询全量重拉。

**根因 A:** `handle_search` 忽略 `UID <range>` 条件,`UID SEARCH UID <last>:*` 返回整箱。
**根因 B:** `handle_uid` 要求 UID 精确匹配邮件,`UID <last>:*` 中 <last> 非确切 UID 时映射成 `0:*` → 全量。

**修复:** SEARCH 按 `>= start && <= end` 过滤;UID 范围收集落在区间内的邮件 seq。

### 10. 重复收件箱

**症状:** 客户端把两个收件箱当独立文件夹分别同步,指针卡住。

**根因:** 投递 SQL `JOIN mailboxes mb ON mb.box_type=1` 把邮件链接到**所有** box_type=1 收件箱;test3 有 `INBOX`(66)+`收件箱`(56)两个。

**修复:** 删除重复的 66,恢复每用户一个收件箱。

---

## 测试

新增 [mime_parser_test.cpp](../../test/unit/mime_parser_test.cpp),构造合法 MIME(单 part、multipart/alternative、multipart/mixed、嵌套、folded header、DKIM h= 干扰、带引号/不带引号 charset、不带引号 boundary)验证 `parse_mime_tree` / `build_bodystructure_tree` / `extract_part_content`。

```bash
ctest -R mime_parser_test   # native
```

## 经验

- MIME 解析边界情况极多,维护手写解析器需持续补回归测试(每修一个 bug 加一个用例)。
- IMAP 对大小写敏感(flag、属性名)要求严格按 RFC 处理。
- 缓存只做读缓存(invalidate on write),数据库是唯一事实源。
