# CS144 TCP 架构与实现说明

本文档面向当前代码库（本仓库 main 分支）的实现，介绍核心组件的职责、相互关系、数据流向、关键算法与实现要点，并给出与代码文件的映射，帮助理解与扩展。

---

## 1. 架构总览

本实现将一个 TCP 端点拆分为三大核心模块，由一个上层协调者统一调度：

- TCPConnection（连接协调者 / 大脑）
  - 文件：`libsponge/tcp_connection.hh`, `libsponge/tcp_connection.cc`
  - 作用：粘合发送与接收两大子系统，对外暴露 `write()`/`read()` 等接口，处理入站段并驱动出站段，管理连接状态（建立、传输、关闭、RST）。

- TCPSender（发送子系统）
  - 文件：`libsponge/tcp_sender.hh`, `libsponge/tcp_sender.cc`
  - 作用：从应用写入的 `ByteStream` 中取数据，按接收端通告窗口切分为 `TCPSegment`，分配序列号，维护在途数据、计时器与重传，直至被 ACK 确认。

- TCPReceiver（接收子系统）
  - 文件：`libsponge/tcp_receiver.hh`, `libsponge/tcp_receiver.cc`
  - 作用：接收网络到来的 `TCPSegment`，校验与投递到 `StreamReassembler`，按序重组成 `ByteStream`，并对外给出下一期望字节（`ackno()`）与可用窗口（`window_size()`）。

配套基础组件：

- ByteStream：`libsponge/byte_stream.hh`, `.cc` —— 有界 FIFO 字节队列，提供 `write/read/end_input/eof/remaining_capacity` 等。
- StreamReassembler：`libsponge/stream_reassembler.hh`, `.cc` —— 将乱序分片按序重组，投递到 `ByteStream`。
- TCPSegment/TCPHeader：`libsponge/tcp_helpers/tcp_segment.hh`, `.cc` / `tcp_header.hh`, `.cc` —— 段与头部的表达、序列化、校验和计算，`length_in_sequence_space()` 计算序列号空间占用（数据字节数 + SYN + FIN）。
- WrappingInt32：`libsponge/wrapping_integers.hh`, `.cc` —— 32 位序列号回绕工具（`wrap/unwrap`）。
- Buffer/BufferList 与解析器：`libsponge/util/*`, `libsponge/tcp_helpers/parser.hh` —— 序列化、反序列化与校验辅助。

模块关系（简图）：

```
Application
   |  write()/read()
   v
TCPConnection  <---- 网络 ---->  对端
  |        ^
  |        |
  v        |
TCPSender  |  (attach ackno/window before send)
  ^        |
  |        v
ByteStream  TCPReceiver --(reassembled)--> ByteStream
```

---

## 2. 数据流与时序

### 2.1 出站路径（发送）

1) 应用层 `write()` → 数据写入 `TCPSender::_stream`（`ByteStream`）。
2) `TCPConnection` 触发 `TCPSender::fill_window()`：
   - 依据接收方窗口（`window_size`，初始可按 1 处理零窗口探测语义），
   - 从 `_stream` 读取不超过 `MAX_PAYLOAD_SIZE` 的负载，
   - 构造 `TCPSegment`，必要时附 `SYN`（首次）与可放入窗口的 `FIN`（在输入流 EOF 时），
   - 设定 `seqno = next_seqno()`，
   - 入队 `_segments_out`（供上层取走发往网络），并记录到 `_outgoing_queue`（发送但未确认队列），更新 `_next_seqno` 与 `_flight_bytes`。
3) `TCPConnection` 在实际发送前，为每个出队段填入当前 `ackno()` 与 `window_size()`（来自 `TCPReceiver`）。

### 2.2 入站路径（接收）

1) 网络到来的 `TCPSegment` 交给 `TCPConnection::segment_received()`。
2) `TCPReceiver::segment_received()`：
   - 校验与解析 `seqno`/`SYN`/`FIN`，将有效负载投递到 `StreamReassembler`，
   - 维护期望下一个字节位置，进而给出 `ackno()`，
   - 根据 `ByteStream` 的剩余容量给出 `window_size()`。
3) 如段带 ACK，由 `TCPConnection` 调用 `TCPSender::ack_received()`：
   - 解除已被确认的在途段，
   - 重置 RTO 与相关计数，
   - 触发 `fill_window()` 尽快发送更多数据。

### 2.3 连接建立与关闭（概要）

- 建立：主动端 `TCPConnection::connect()` 触发 `TCPSender` 发送 `SYN`，对端回 `SYN+ACK`，本端回 `ACK`。
- 关闭：应用 `end_input_stream()` → 发送 `FIN`；对端回 ACK，最终对端也发 `FIN`，本端 ACK，进入 `TIME_WAIT`（由 `TCPConnection::tick()` 驱动时间）。
- 异常：`RST` 收/发由 `TCPConnection::set_rst_state()` 处理，立刻终止连接。

---

## 3. 逐组件实现要点

### 3.1 TCPSender（`libsponge/tcp_sender.*`）

关键成员（当前实现中常见命名）：
- `_stream`：出站 `ByteStream`。
- `_segments_out`：待由连接层取走并发送的段队列。
- `_outgoing_queue`：记录“已发送但未确认”的段（含其起始绝对序列号），用于超时重传与 ACK 处理。
- `_next_seqno`：下一个将要发送的绝对序列号（64 位）。
- `_flight_bytes`：在途字节数（序列号空间计数；SYN/FIN 各算 1）。
- `_last_win_sz`：最近一次接收方通告的窗口大小（零窗口时发送方可按 1 进行探测）。
- `_syn/_fin`：是否已发送过 SYN/FIN。
- `RTO/计时器`：`_timeout`（当前 RTO）、`_initial_retransmission_timeout`（初始 RTO）、`_time_wait`（自上次发送/超时起累计 ms）、`_consecutive_retransmissions_count`（连续重传次数）。

核心方法：
- `fill_window()`：
  - 确保首次发送含 `SYN`；
  - 在窗口允许下，尽可能装填负载（不超过 `MAX_PAYLOAD_SIZE` 与剩余窗口），
  - 若输入流 EOF 且窗口尚有 1 个序列号空间，则附加 `FIN`；
  - 将段推入 `_segments_out`（供连接层发送），并记录到 `_outgoing_queue`，更新 `_flight_bytes/_next_seqno`；
  - 首包或清空后首发时重置 RTO 计数器。
- `ack_received(ackno, window_size)`：
  - `unwrap(ackno, _isn, _next_seqno)` 得到绝对确认号，若确认号超前于 `_next_seqno` 判无效（返回 false）；
  - 从 `_outgoing_queue` 头部起移除所有“完全被确认”的段，减少 `_flight_bytes`，重置 RTO 与计时器；
  - 清零 `_consecutive_retransmissions_count`，更新 `_last_win_sz`，调用 `fill_window()` 继续发包。
- `tick(ms)`：
  - 若无在途包则直接返回；
  - 累计 `_time_wait`，若 `>= _timeout`：重传队头段入 `_segments_out`，若对端窗口 `> 0` 则执行指数退避（`_timeout *= 2`），重置 `_time_wait`，并累计 `_consecutive_retransmissions_count`。
- 其他：`bytes_in_flight()`、`send_empty_segment()`（发送纯 ACK/保活）、`consecutive_retransmissions()`（上报连续重传计数）。

### 3.2 TCPReceiver（`libsponge/tcp_receiver.*`）

- 维护 ISN（首个 `SYN`）、期望下一个绝对字节序号、`StreamReassembler` 与输出 `ByteStream`。
- `segment_received()`：根据 TCP 序列号规则将负载映射为数据流偏移并投递给重组器；若收到 `FIN`，在流达到 EOF 条件时关闭输出端。
- `ackno()`：若未见 `SYN`，无确认号；否则为“已连续按序接收的末尾 + 1”，并计入 `SYN/FIN` 的序列号占位。
- `window_size()`：输出流剩余容量（决定对端可再发多少字节）。

### 3.3 TCPConnection（`libsponge/tcp_connection.*`）

- 责任：
  - 入站：`segment_received()` 将段交给 `_receiver`；若带 ACK，再交 `_sender.ack_received()`；必要时触发回复（含 RST/纯 ACK）。
  - 出站：应用 `write()` → `_sender.stream_in().write()` → `_sender.fill_window()`；随后从 `_sender.segments_out()` 取段，**在发送前**填入 `_receiver.ackno()` 与 `_receiver.window_size()` 并发往网络。
  - 生命周期：`connect()` 发 SYN；`end_input_stream()` 发 FIN；`tick()` 驱动 `_sender.tick()` 与可能的超时/`TIME_WAIT`；`set_rst_state()` 处理异常重置；`active()` 汇报连接仍是否存活。
- 说明：当前仓库附件显示 `tcp_connection.cc` 仍为 Lab 4 的占位（dummy），实际实现需按上述职责完善。

---

## 4. 序列号与序列号空间

- `length_in_sequence_space()`：段在序列号空间占用 = 负载字节数 + `SYN?1:0` + `FIN?1:0`。
- 初始序列号（ISN）：由发送端随机或固定（测试）选取；首个 `SYN` 使用 ISN，本段的第一个数据字节序号为 `ISN+1`。
- `WrappingInt32`：
  - `wrap(uint64_t abs, WrappingInt32 isn)`：绝对序号 → 32 位相对序号。
  - `unwrap(WrappingInt32 rel, WrappingInt32 isn, uint64_t checkpoint)`：相对序号 → 最接近 checkpoint 的绝对序号。

---

## 5. 重传计时与指数退避

- 发送后若未确认，保持在 `_outgoing_queue`；`tick()` 累计时间，超时则重传队头段。
- 若对端窗口 `> 0`，每次超时将 RTO 加倍（指数退避），直到收到新的有效 ACK 将 RTO 重置为初始值。
- 窗口为 0 的情形可使用“零窗口探测”（本实现中通过 `std::max(1, window)` 语义保证仍能探测）。

---

## 6. 典型时序（简化 ASCII）

### 6.1 出站（含 ACK 附带）
```
App              TCPConnection         TCPSender            TCPReceiver/Peer
 |  write()           |                    |                        
 |------------------->| stream_in.write    |                        
 |                    | fill_window()      | build segments         
 |                    | dequeue seg        |                        
 |                    | attach ack/win <---| ackno(), window_size() 
 |                    | send to network ---> Peer                    
```

### 6.2 入站（含重组与 ACK 驱动）
```
Peer ---- segment ----> TCPConnection -> TCPReceiver.segment_received()
                                    \-> if ACK: TCPSender.ack_received()
TCPConnection pulls from TCPSender.segments_out(), attaches ack/win, sends
```

---

## 7. 代码清单与导航

- 发送：`libsponge/tcp_sender.hh`, `libsponge/tcp_sender.cc`
- 接收：`libsponge/tcp_receiver.hh`, `libsponge/tcp_receiver.cc`
- 连接：`libsponge/tcp_connection.hh`, `libsponge/tcp_connection.cc`
- 段/头/序列化：`libsponge/tcp_helpers/tcp_segment.*`, `libsponge/tcp_helpers/tcp_header.*`
- 重组与流：`libsponge/stream_reassembler.*`, `libsponge/byte_stream.*`
- 序列号回绕：`libsponge/wrapping_integers.*`
- 工具：`libsponge/util/*`
- 测试：`tests/`（各 Lab 的 Doctest 场景）

---

## 8. 运行与测试（简要）

- 构建：在 `build/` 下使用 CMake/Make（本仓库脚手架已就绪）。
- 实验检查：`make check_lab0/1/2/3/4` 逐步验证各实验。

---

## 9. 扩展建议与注意事项

- 在 `TCPConnection` 中补全：`segment_received`、`connect`、`tick`、`end_input_stream`、`attach_with_ack_and_win` 等，确保严格遵循 RFC 行为与课程要求。
- 注意边界：SYN/FIN 占位、乱序重组窗口、零窗口与探测、重复 ACK 的处理、连接终止状态机（`TIME_WAIT`/`RST`）。
- 为易测性与可维护性，保持发送/接收与连接层的职责清晰，尽量通过 `segments_out()` 与 `ackno()/window_size()` 解耦。
