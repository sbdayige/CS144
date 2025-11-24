# CS144 TCP/IP 协议栈项目总结与架构文档

本文档对当前实现的 TCP/IP 协议栈项目进行全面的总结。本项目构建了一个运行在用户态的完整网络协议栈，涵盖了从链路层以太网帧的处理、网络层 IP 数据报的路由转发，到传输层 TCP 可靠字节流传输的核心功能。

---

## 1. 项目架构概览

本项目采用经典的分层架构，各组件职责清晰，严格遵循 TCP/IP 协议规范。

### 1.1 架构分层图

```text
+---------------------+
|   Application Layer |  <-- ByteStream (读/写接口)
+---------------------+
|   Transport Layer   |  <-- TCPConnection (协调者)
|                     |      |-- TCPSender (发送、重传、流控)
|                     |      |-- TCPReceiver (接收、重组、确认)
+---------------------+
|   Network Layer     |  <-- Router (路由表、LPM 转发)
+---------------------+
|   Link Layer        |  <-- NetworkInterface (ARP、以太网封装)
+---------------------+
```

### 1.2 核心组件清单

1.  **`ByteStream`**: 一个有限容量的字节流管道，作为应用层与传输层之间的数据缓冲区。
2.  **`StreamReassembler`**: 负责处理乱序到达的数据片段，将其重组为连续的字节流。
3.  **`TCPReceiver`**: 处理入站 TCP 段，计算确认号 (ackno) 和接收窗口 (window size)。
4.  **`TCPSender`**: 处理出站数据，负责分段、序列号管理、超时重传 (RTO) 和指数退避。
5.  **`TCPConnection`**: 整个 TCP 协议的“大脑”，封装了 Sender 和 Receiver，管理连接状态机 (三次握手、四次挥手、RST)。
6.  **`NetworkInterface`**: 实现了 IP 与以太网的适配，核心功能是 ARP 协议（地址解析）和帧的封装/解封装。
7.  **`Router`**: 实现了多接口的 IP 数据报转发，核心算法是最长前缀匹配 (LPM)。

---

## 2. 核心功能实现详解

### 2.1 传输层 (Transport Layer)

传输层的目标是在不可靠的 IP 网络上提供**可靠的字节流服务**。

*   **可靠性保障 (`TCPSender`)**:
    *   **切片**: 将 `ByteStream` 中的数据切分为不超过 MSS 的 TCP 段。
    *   **追踪**: 维护 `_outgoing_queue` 记录所有“在途” (In-flight) 但未确认的段。
    *   **重传**: 内部维护一个计时器。如果超过 RTO (Retransmission Timeout) 未收到 ACK，则重传最早的段，并执行**指数退避** (RTO * 2) 以避免网络拥塞。
    *   **流控**: 严格遵守接收方通告的 `window_size`，不发送超过窗口大小的数据（零窗口探测除外）。

*   **有序性保障 (`TCPReceiver`)**:
    *   **重组**: 使用 `StreamReassembler` 缓存乱序到达的段 (unassembled bytes)。只有当数据填补了当前的空缺 (hole) 时，才写入 `ByteStream` 供应用读取。
    *   **反馈**: 根据已成功重组的字节数计算 `ackno`，根据 `ByteStream` 的剩余容量计算 `window_size`。

*   **连接管理 (`TCPConnection`)**:
    *   协调 Sender 和 Receiver。例如，在发送数据段时，自动附带上 Receiver 当前的 ACK 和 Window 信息（捎带确认）。
    *   处理连接的建立 (SYN 交换) 和断开 (FIN 交换)，以及异常情况下的复位 (RST)。

### 2.2 网络接口层 (Network Interface Layer)

该层解决了**逻辑地址 (IP) 到物理地址 (MAC) 的映射**问题。

*   **ARP 协议实现**:
    *   维护一个 ARP 缓存表 (`_arp_table`)，记录 `IP -> MAC` 的映射及 TTL。
    *   **发送时**: 如果目标 MAC 未知，先将数据报缓存 (`_waiting_datagrams`)，并广播 ARP Request。收到 Reply 后，更新缓存并发送滞留的数据报。
    *   **接收时**: 能够响应针对本机 IP 的 ARP Request，回复 ARP Reply。

### 2.3 路由层 (Routing Layer)

该层实现了 IP 数据报在不同网络间的**转发**。

*   **路由表**: 存储 `RouteEntry` (前缀、长度、下一跳、出接口)。
*   **转发逻辑 (`Router`)**:
    1.  **TTL 检查**: 丢弃 TTL <= 1 的包，防止路由环路。
    2.  **最长前缀匹配 (LPM)**: 遍历路由表，找到与目标 IP 匹配且前缀最长的那条规则。
    3.  **下一跳确定**: 如果规则指定了下一跳 (Gateway)，则发往该地址；否则视为直连网络，直接发往目标 IP。

---

## 3. TCP/IP 数据传输全流程演示

为了串联上述组件，我们以一个具体的场景为例：**主机 A 向主机 B 发送一段数据 "Hello"，中间经过路由器 R**。

### 阶段一：主机 A 产生并发送数据 (Transport -> Link)

1.  **应用层**: 用户调用 `write("Hello")`。数据进入 A 的 `ByteStream`。
2.  **传输层 (`TCPSender`)**:
    *   `fill_window` 被触发，从流中读取 "Hello"。
    *   封装成 `TCPSegment`，分配序列号 (SEQ=100)，设置 SYN/FIN 标志（视情况而定）。
    *   `TCPConnection` 将段交给下层，并附带上 A 当前的 ACK=0, Win=64000。
3.  **网络层**: 封装成 IP 数据报 (Src: IP_A, Dst: IP_B)。
4.  **链路层 (`NetworkInterface`)**:
    *   调用 `send_datagram`。下一跳是默认网关 (路由器 R)。
    *   查 ARP 表：假设已知 R 的 MAC 地址。
    *   封装成以太网帧 (Src: MAC_A, Dst: MAC_R, Type: IPv4)。
    *   **发送到物理线路**。

### 阶段二：路由器 R 转发 (Network Layer)

1.  **接收 (`NetworkInterface`)**: R 的入接口收到帧，校验目的 MAC 是自己，解包出 IP 数据报。
2.  **路由 (`Router`)**:
    *   调用 `route_one_datagram`。
    *   TTL 减 1。
    *   查路由表：匹配到目标 IP_B 所在的子网，确定出接口是 eth1，下一跳是 IP_B (直连)。
3.  **发送 (`NetworkInterface`)**:
    *   R 的 eth1 接口调用 `send_datagram`。
    *   查 ARP 表：假设**未知** IP_B 的 MAC 地址。
    *   **ARP 过程**:
        *   缓存 IP 数据报。
        *   广播 ARP Request (Who has IP_B?)。
        *   B 收到并回复 ARP Reply (IP_B is at MAC_B)。
        *   R 更新 ARP 表，取出滞留的数据报。
    *   封装新帧 (Src: MAC_R_eth1, Dst: MAC_B, Type: IPv4)。
    *   **发送到物理线路**。

### 阶段三：主机 B 接收并处理 (Link -> Transport)

1.  **链路层 (`NetworkInterface`)**: B 收到帧，校验 MAC，解包出 IP 数据报。
2.  **传输层 (`TCPConnection`)**:
    *   收到 IP 数据报中的 TCP 段。
    *   **`TCPReceiver`**:
        *   检查 SEQ=100。
        *   `StreamReassembler` 将 "Hello" 放入正确位置。
        *   数据按序，写入 B 的入站 `ByteStream`。
        *   更新 `ackno` 为 100 + len("Hello")。
    *   **`TCPSender`**:
        *   处理段中的 ACK 信息（如果有），更新 B 发送方的窗口状态。
3.  **应用层**: B 的用户调用 `read()`，读出 "Hello"。

### 阶段四：确认 (ACK)

1.  B 的 `TCPConnection` 稍后会发送一个段（可能是纯 ACK，也可能携带 B 发给 A 的数据）。
2.  该段的头部会包含 `ackno` (确认收到 "Hello")。
3.  经过网络回到 A。
4.  A 的 `TCPSender` 收到 ACK，将 "Hello" 对应的段从 `_outgoing_queue` 中移除，**交易完成**。

---

## 4. 总结

通过本项目，我们从零开始构建了一个功能完备的协议栈。
*   **NetworkInterface** 让我们可以与物理网络交互。
*   **Router** 让我们可以跨越网络边界。
*   **TCP 模块** (Sender/Receiver/Connection) 让我们可以无视底层网络的丢包、乱序和不可靠，为上层应用提供了一个**可靠、有序、流式**的通信环境。

这不仅实现了 TCP/IP 的核心功能，也展示了分层设计的强大之处：每一层只关注自己的职责，通过定义良好的接口与上下层交互，共同完成了复杂的网络通信任务。
