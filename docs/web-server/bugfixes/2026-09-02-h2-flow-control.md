# H2 大文件流控与 content-length 的坑（2026-09-02，已修已提 d83279d）

> 三个协议级根因 + 两个跨协议复用 HttpRequest 的坑。共同主题：**H1 的发送惯性在 H2 不成立**
> —— H2 没有"靠断连定结尾"的宽松，长度/帧大小/窗口全是硬约束。

## 1. 单帧超 MAX_FRAME_SIZE → FRAME_SIZE_ERROR（8MB 首块 0B 元凶）
- 默认单帧上限 **16384**，发超过即对端判 FRAME_SIZE_ERROR 断连。
- 8MB 首块 drain 直接发 65535B > 16384 → curl 收到即断，0 字节。
- 修：`max_frame_size_` + `drain_stream` 单帧切成 `min(流窗, 连接窗, 此值)` + 解析对端 SETTINGS_MAX_FRAME_SIZE。

## 2. 单调流 ID 门控误杀后续帧
- 曾对**所有**流级帧套 `stream_id <= last_client_stream_` → 同流上的 WINDOW_UPDATE/DATA
  复用 ID 被判"重复开流" → PROTOCOL_ERROR，发起首批 65535B 就崩。
- 修：只在 **HEADERS 开新流**时校验单调递增；WU/DATA/CONTINUATION 复用合法。

## 3. SETTINGS_INITIAL_WINDOW_SIZE 赋给连接窗
- 曾 `conn_send_window_ = val`；但该 SETTINGS 管**每流**发送窗，连接窗只由连接级 WU 抬升。
- 修：按 delta 调整在建流 `send_window`，新流从 `peer_stream_window_` 起；连接窗不动。

## 4. 错误响应 content-length:-1
- `HttpResponse` 默认 -1（H1 语义"由 body.size() 推"）；H2 直接发到线上被拒。
- 修：错误内联体分支显式设 `content_length = body.size()`。
- 教训：跨协议复用类型，字段默认值里隐含的"某协议推导"约定会泄漏到另一协议，须各协议自洽。

## 5. content-length≠0 却不发 DATA → PROTOCOL_CLOSED
- 只发 HEADERS(content-length:11) 不发 DATA → RFC 9113 要求 DATA 总和等于 content-length → 硬错。
- 修：错误体作真实 DATA 随 HEADERS 发出。（HEAD 请求是规范豁免，故 200 HEAD 不报。）

## 验证
io_thread=1：60K/100K/8MB curl 全数逐字节一致。日后整机多流 e2e 待自动化（见 `../local/TODO.md`）。