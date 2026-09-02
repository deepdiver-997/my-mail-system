# Web 服务器分层心智模型：共用什么、各自管什么

> 一句话：**可复用的是"HTTP 消息状态机"，不是"传输层流抽象"。**
> 前者现在就该抽出来让 H1/H2 共用；后者等 QUIC 真立项再单独建在传输层、跟 TCP 严格分开。

## 三层切断

```
wire 层（协议专属）：字节 ↔ 帧/流/请求消息
   H1：按行读 → Message（Content-Length / chunked 精确取）
   H2：按帧读（9B 头）→ codec 按 stream_id 去复用 → 请求消息
   H3(未来)：每条 QUIC 流按流内字节积满请求

消息层（协议无关，共用）：HttpRequest → HttpResponse
   MessageProcessor：纯函数，路由/防穿越/定长/ctype。不碰 I/O、不持 session。

会话层（承载传输，各自一个类）：socket 读循环 + 发送 + 生命周期
   HttpSession[Con]（H1）   /   H2Session[Con]（H2）   /   QuicSession[Con]（未来）
```

## 关键决策与理由

### 1. H1/H2 能不能共用读循环？—— 不能，读粒度本质不同
- H1 按**行**，H2 按**帧**。`has_buffered_input()/extract_one_line()` 两协议各自写各自的。
- 硬塞进"一个共享 base + 一个 Codec" → Codec 要么塞满两协议读循环差异（它就是 session 换名），
  要么薄到没意义 —— **leaky abstraction**。所以**不抽**"共享读循环"。

### 2. 真正成立的共用缝 = MessageProcessor（纯 request→response）
- H1 每连接一个、H2 每条流一个、H3 每条 QUIC 流一个，喂的都是**同一份规范化 HttpRequest**。
- 它只做"路由 + 分类"（status/ctype/length/full_path），**体不入内存**：
  H1 走 sendfile/流式，H2 读入走流控分片，各自决定"体怎么送"。

### 3. H2 的 wire→message 独立成 H2Codec（这是给 H3 留的缝）
- `codec/h2_codec.{h,cpp}`：帧/流去复用 + 每流请求头装配 + HPACK 解码 → 归一化 HttpRequest。
- H2Session 只剩 socket + 流控窗口 + 发送。会话层对协议无关（"落下 H2 就喂 MessageProcessor"）。
- H3Codec 实现同形态接口，MessageProcessor 原样复用。

### 4. 传输层要不要暴露"流"抽象给 QUIC？
- **不要给 TCP 加流**。H2 的 stream 是应用层（帧头里 31bit 字段）；QUIC 的 stream 是传输层
  （UDP 之上真正独立的可靠字节流）。两者不是同一层的东西，塞进同一抽象就漏。
- H3 真做时：加一个**能承载多流的全新传输端**（与 TCP 平级），复用点只在 HttpMessage 层。

## 所有权：单向，无 shared_ptr 循环
```
连接 瞬态pin(在途 async_op 捕获 self) → session(成员) → 流/流控态
MessageProcessor 不持 session（调用方栈上活着，传 this/引用）
→ 无循环、无手动拆引、无 leak。（H2 曾踩过 map 悬垂，见 bugfixes）
```