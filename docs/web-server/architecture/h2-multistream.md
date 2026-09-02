# HTTP/2 多流会话模型：两层状态机 + 流控三分

> 结构断点：HTTP/1.1 是"每连接一个 FSM 串行"，HTTP/2 把一根连接切成 N 条逻辑**流（stream）**，
> 帧按 `stream_id` 去复用并行传输。这逼着一对连接从"一个 FSM"拆成两层。

## 两层模型

```
连接级 FSM（ConnState）           整个 TCP 连接一份
  PREFACE → OPEN → CLOSED
  帧定界 / SETTINGS+ACK / PING+ACK / GOAWAY
  连接级发送窗口 conn_send_window_ / 对端最大帧大小 max_frame_size_
  （HPACK decoder 属连接级，但收在 codec）

流注册表 map<stream_id, H2Stream>   每条流一份
  每流 FSM：IDLE → OPEN → HALF_CLOSED → CLOSED
  请求头装配/去复用 → 归 H2Codec；流控/发送窗 → 归 H2Session
```

## 流控三个窗口（各自独立的坑）

1. **连接级发送窗 `conn_send_window_`**：初始 65535，只由**连接级 WINDOW_UPDATE（stream 0）**抬升。
   与任何 SETTINGS 无关。
2. **每流发送窗 `send_window`**：新流初始 = 对端 `SETTINGS_INITIAL_WINDOW_SIZE`，随流级 WINDOW_UPDATE 抬升。
   **易错**：这个 SETTINGS 管"每流"，不是连接窗。曾经把它赋给 `conn_send_window_` → 大文件发完首批就卡。
3. **单帧上限 `max_frame_size_`**：默认 16384，被对端 SETTINGS_MAX_FRAME_SIZE 覆盖（只升不降，合法区间 16384~2^24）。
   **易错**：发 DATA 单帧超它 → 对端 FRAME_SIZE_ERROR（8MB 首块 0B 的元凶）。

`drain_stream` 每帧长度 = `min(流窗, 连接窗, 对端最大帧大小)`。

## 发送路径（一次请求）
1. codec 收齐 HEADERS+END_STREAM → 解码 → 归一化 `HttpRequest` → 会话 `serve_stream`。
2. MessageProcessor 路由 → 响应头（:status/content-type/content-length）+ 体。
3. 大 body → `pending_body` → `drain_stream` 分片发 DATA，按窗口/帧上限切片；
   窗口耗尽等 WINDOW_UPDATE 续发，发完 END_STREAM 标 done（安全点 GC）。
4. 出站 `send_frame` 队列串行 flush（不走 SessionBase 请求-响应写缓冲，否则响应滞留）。

## read 循环必续：H2 弃用 SessionBase 写钩子 → 自己负责 re-arm
`process_read()` 末尾必须 `do_async_read()`，否则只处理首段、多 TCP 段请求挂死。

## 并发前提（务必看 bugfix/shared-io-context）
H2 连续多帧 pumping，对"单连接只被一个 io 线程调度"极其敏感。框架曾因
`vector<shared_ptr>(count, make_shared)` 让所有 io 线程共享一个 io_context → 单连接被多线程
调度 → `out_pending_` 竞态 UAF。已修（每线程独立 context）。SMTP/IMAP/POP3 因单次读写未暴露。