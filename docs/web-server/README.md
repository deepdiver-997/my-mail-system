# Web 服务器（HTTP/1.1 + HTTP/2）业务文档

基于框架搭的 web 服务器业务文档。框架/基础设施类文档在 [`../architecture/`](../architecture/)，
构建运维在 [`../build-deploy/`](../build-deploy/)。**目前是验证框架可用性的脚手架
（scaffold）**：可服务真实静态文件，但不是生产级完整 HTTP 服务器。

## 模块范围

```
web_server/
  http_types.hpp        HttpRequest / HttpResponse（跨 H1/H2 共享）
  http_parser.{h,cpp}   resolve_safe_path 防穿越 / mime / 请求行&头解析
  message_processor.*   协议无关 request→response 纯函数（路由/定长/ctype，体不入内存）
  http_fsm / http_session   HTTP/1.1（按行读，keep-alive）
  h2/                    HTTP/2 多流会话（H2Session = socket+流控+发送）
  codec/h2_codec.*       HTTP/2 wire→message（帧去复用+头装配+HPACK），成败在于这一层
  h2_framer / h2_hpack / h2_huffman_table  二进制帧 & HPACK 编解码
```

## 阅读顺序（新人建议）

1. **architecture/layering.md** — 分层心智模型：**哪部分是各协议共用的
   （MessageProcessor），哪部分各协议专属（wire 层）**。这是理解整个模块的核心。
2. **architecture/h2-multistream.md** — HTTP/2 多流会话：为什么跟 H1"重做状态机"、
   连接级 FSM + 流注册表两层模型、流控三分。
3. **architecture/h2-codec.md** — wire→message 独立成 codec 的切法，H3/QUIC 的复用处。
4. 按需深入 bug 复盘。

## 架构

| 文档 | 内容 |
|------|------|
| [architecture/layering.md](architecture/layering.md) | 分层：共用 vs 各自（MessageProcessor / wire / 会话） |
| [architecture/h2-multistream.md](architecture/h2-multistream.md) | H2 连接级 FSM + 流注册表、流控窗口模型 |
| [architecture/h2-codec.md](architecture/h2-codec.md) | H2Codec（wire→message）切法与 H3 复用缝 |

## Bug 复盘

| 文档 | 内容 |
|------|------|
| [bugfixes/2026-09-02-h2-flow-control.md](bugfixes/2026-09-02-h2-flow-control.md) | H2 大文件流控三 root cause + content-length 两个坑 |
| [bugfixes/2026-09-02-shared-io-context.md](bugfixes/2026-09-02-shared-io-context.md) | 框架 vector+shared_ptr 让所有线程共享 io_context → H2 并发 UAF |

> 纯函数层（MessageProcessor / framer / hpack / codec）已有单测 `h2_web_test`（39 断言）。
> 整机多流 e2e 仍手动（见 `../local/TODO.md`）。新 bugfix 按 `YYYY-MM-DD-简短.md` 命名。