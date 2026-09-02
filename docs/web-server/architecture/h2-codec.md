# H2Codec：把 HTTP/2 的 wire→message 独立成层

> 动机：把"字节怎么变成一条请求消息"从会话里剥出来，让 H2Session 只留
> socket + 流控 + 发送，也让会话层将来对 H3/QUIC 复用同一份"喂 MessageProcessor"逻辑。

## 职责边界

```
codec/h2_codec.{h,cpp}          （纯，可单测）
  帧/流级去复用
  每流请求头装配：HEADERS/CONTINUATION/DATA 累积 header_block，END_HEADERS+END_STREAM 判定
  流 ID 门控：偶数 ID 非法；只有 HEADERS 开新流才校验"客户端单调递增"（后续 WU/DATA 复用合法）
  PRIORITY/PADDED 前缀剥离
  连接级 HPACK 解码 → 归一化 HttpRequest
  产出：Result{ protocol_ok, error(PROTOCOL/COMPRESSION), optional<HttpRequest> request }

H2Session
  socket 读循环（SessionBase 帧式覆写）
  流控窗口（send_window / conn_send_window_ / peer_stream_window_ / max_frame_size_）
  出站发送（drain_stream / send_frame / flush_out）
  RST / WINDOW_UPDATE / serving 组装
  serve_stream(req)  直接吃 codec 抛的 HttpRequest → MessageProcessor
```

## 关键：流控类帧留在 session，头装配进 codec
- RST / WINDOW_UPDATE / PRIORITY 涉及会话的流控/生命周期 → session 自处理。
- HEADERS / CONTINUATION / DATA 参与"把请求拼出来" → codec。
- 连接级 SETTINGS：side 解流控窗，codec 配 HPACK 表（`apply_header_table_size`）。

## 为什么这里"不显得漏"
一条清晰边界：**codec 管"字节→一条请求消息"，session 管"窗口 + 落盘发送"**。流的多少、
帧的形态全在 codec 侧，会话对协议无关。

## H3/QUIC 复用缝
将来 H3Codec（每 QUIC 流喂字节 → 请求消息）实现同形态；`MessageProcessor` 原样复用。
**不要**给 TCP 加"传输层流"抽象来迁就 H2（stream 是应用层概念，见 layering.md）。

## 单测
`test/unit/h2_web_test.cpp` 覆盖：完整/分片 HEADERS→归一化请求；偶数流 ID、非单调重复开流、
孤儿 CONTINUATION → PROTOCOL_ERROR；hpack encode↔decode 逐一致；framer 往返。