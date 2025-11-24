# Router 组件详解与实现

本文档详细介绍了 CS144 项目中 `Router`（路由器）组件的作用、在 TCP/IP 协议栈中的角色，以及具体的实现逻辑。

---

## 1. 组件作用与角色

### 1.1 核心作用
`Router` 是网络层（Network Layer，Layer 3）的核心设备。它的主要职责是**将 IP 数据报（Internet Datagrams）从一个网络接口转发到另一个网络接口**，从而实现不同网络之间的互联。

简而言之，如果说 `NetworkInterface` 负责在**同一个局域网**内通过 MAC 地址传输数据，那么 `Router` 就负责在**不同的网络**之间通过 IP 地址传输数据。

### 1.2 在 TCP/IP 传输中的角色
在一次完整的 TCP 传输流程中，`Router` 扮演着“交通枢纽”的角色：

1.  **源主机 (Source)**: 产生 TCP 段，封装成 IP 数据报。如果目标 IP 不在本地子网，源主机将数据报发给默认网关（即路由器）。
2.  **路由器 (Router)**:
    *   **接收**: 从某个网络接口（如 eth0）收到 IP 数据报。
    *   **决策**: 查看数据报的目标 IP 地址，查询**路由表**，决定该数据报应该从哪个接口（如 eth1）发出去，以及下一跳是谁。
    *   **转发**: 修改数据报（如减少 TTL），通过选定的接口发送给下一跳。
3.  **目标主机 (Destination)**: 最终接收 IP 数据报，解包交给 TCP 层处理。

没有路由器，互联网将只是一个个孤立的局域网，无法形成全球互联的网络。

---

## 2. 实现细节

### 2.1 数据结构设计 (`router.hh`)

为了支持路由功能，我们在 `Router` 类中增加了以下数据结构：

```cpp
struct RouteEntry {
    uint32_t route_prefix;      // 路由前缀 (例如 192.168.1.0)
    uint8_t prefix_length;      // 前缀长度 (例如 24，对应子网掩码 255.255.255.0)
    std::optional<Address> next_hop; // 下一跳地址 (如果是直连网络则为空)
    size_t interface_num;       // 出接口索引
};

std::vector<RouteEntry> _routing_table{}; // 路由表
```

*   **`_routing_table`**: 这是一个线性表，存储了所有的路由规则。每一条规则告诉路由器：“如果你看到目标 IP 匹配这个前缀，就把它发往这里”。

### 2.2 添加路由 (`add_route`)

`add_route` 函数用于填充路由表。实现非常直接：将传入的参数（前缀、长度、下一跳、接口）打包成 `RouteEntry` 并追加到 `_routing_table` 中。

```cpp
void Router::add_route(...) {
    _routing_table.push_back({route_prefix, prefix_length, next_hop, interface_num});
}
```

### 2.3 路由转发逻辑 (`route_one_datagram`)

这是路由器的核心逻辑，实现了 **最长前缀匹配 (Longest Prefix Match, LPM)** 算法。

当路由器需要转发一个数据报时，执行以下步骤：

#### 步骤 1: TTL 检查 (Time To Live)
为了防止数据报在网络中无限循环，每个 IP 包都有一个 TTL 字段。
*   如果 `dgram.header().ttl <= 1`，说明该包寿命已尽，路由器将其**丢弃**（不转发）。
*   否则，将 TTL 减 1。

```cpp
if (dgram.header().ttl <= 1) return;
dgram.header().ttl--;
```

#### 步骤 2: 最长前缀匹配 (LPM)
路由器遍历路由表中的每一条规则，寻找与目标 IP 地址匹配的规则。如果有多条规则匹配，选择**前缀长度最长**的那条（因为它最精确）。

*   **匹配逻辑**: 利用位运算。
    *   构造掩码：`mask = (~0u << (32 - prefix_length))`。例如 /24 的掩码就是高 24 位为 1。
    *   比较：`(dst_ip & mask) == (route_prefix & mask)`。
*   **特殊情况**: 默认路由（0.0.0.0/0）的长度为 0，掩码为 0，能匹配任何 IP。

```cpp
auto best_match = _routing_table.end();
int max_prefix_len = -1;

for (auto it = _routing_table.begin(); it != _routing_table.end(); ++it) {
    // 计算掩码 (注意处理 prefix_length 为 0 的情况)
    uint32_t mask = (it->prefix_length == 0) ? 0 : (~0u << (32 - it->prefix_length));
    
    if ((dgram.header().dst & mask) == (it->route_prefix & mask)) {
        // 找到匹配，检查是否更长
        if (it->prefix_length > max_prefix_len) {
            max_prefix_len = it->prefix_length;
            best_match = it;
        }
    }
}
```

#### 步骤 3: 转发
如果找到了匹配的路由规则 (`best_match`)：
1.  **确定下一跳**:
    *   如果规则中指定了 `next_hop`（说明是发给下一个路由器），则下一跳地址就是该地址。
    *   如果 `next_hop` 为空（说明目标 IP 就在直连的局域网上），则下一跳地址就是数据报的目标 IP 地址 (`dgram.header().dst`)。
2.  **发送**: 调用对应接口 (`_interfaces[interface_num]`) 的 `send_datagram` 方法将数据报发出去。

```cpp
if (best_match != _routing_table.end()) {
    const Address next_hop = best_match->next_hop.has_value() 
                           ? best_match->next_hop.value() 
                           : Address::from_ipv4_numeric(dgram.header().dst);
    
    _interfaces[best_match->interface_num].send_datagram(dgram, next_hop);
}
```

---

## 3. 总结

通过上述实现，`Router` 组件能够正确地处理 IP 数据报的转发，它是连接不同子网的桥梁，确保了数据能够在复杂的网络拓扑中找到正确的路径到达目的地。结合之前的 `NetworkInterface`（负责单跳传输）和 `TCPConnection`（负责端到端可靠传输），我们构建了一个功能完整的微型 TCP/IP 协议栈。
