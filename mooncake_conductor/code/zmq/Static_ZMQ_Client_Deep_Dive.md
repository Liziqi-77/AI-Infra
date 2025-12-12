# Static ZMQ Client 代码逐行深度解析

本文档对 `pkg/cache/kvcache/static_zmq_client.go` 进行逐行解析。该版本专门为静态部署环境优化，移除了冗余逻辑，代码更加精简。

---

## 1. 结构体定义

```go
17| type StaticZMQClient struct {
...
30| 	// Lifecycle
31| 	ctx    context.Context
32| 	cancel context.CancelFunc
33| 	wg     sync.WaitGroup
34| }
```

*   **简化点**: 移除了 `reconnectDelay`、`reconnectTicker` 和 `metrics` 字段。
*   **设计意图**: 静态场景下，重连策略是固定的，不需要在结构体中维护动态变化的延迟状态。

## 2. 构造函数 (NewStaticZMQClient)

```go
37| func NewStaticZMQClient(config *ZMQClientConfig, handler EventHandler) *StaticZMQClient {
38| 	ctx, cancel := context.WithCancel(context.Background())
39| 	return &StaticZMQClient{
...
42| 		lastSeq:      -1,
...
45| 	}
46| }
```

*   **Line 42**: 初始化 `lastSeq` 为 -1，表示尚未接收到任何消息。

## 3. 主循环 (loop) - 逻辑重构

静态版将连接管理和事件消费合并到了一个清晰的循环中。

```go
82| func (c *StaticZMQClient) loop() {
...
86| 	ticker := time.NewTicker(c.config.ReconnectDelay)
...
89| 	for {
...
98| 		// 1. If disconnected, wait for ticker then try to reconnect
99| 		if !c.isConnected() {
100| 			select {
101| 			case <-c.ctx.Done():
102| 				return
103| 			case <-ticker.C:
104| 				if err := c.Connect(); err != nil {
105| 					slog.Error("Reconnect failed", ...)
106| 				} else {
107| 					// Reconnected! Request replay from last known sequence
108| 					lastSeq := c.getLastSequence()
109| 					_ = c.requestReplay(lastSeq + 1)
110| 				}
111| 			}
112| 			continue
113| 		}
```

*   **Line 86**: **固定间隔定时器**。使用配置中的 `ReconnectDelay`（例如 1秒）作为固定重试间隔。
*   **Line 99**: **状态驱动**。如果当前未连接，则阻塞等待定时器触发，然后尝试连接。
*   **Line 109**: **自动重放**。一旦重连成功，立即请求从 `lastSeq + 1` 开始重放。这是静态客户端的一个增强特性——自动补齐数据。

```go
116| 		// 2. If connected, consume events
117| 		if err := c.consume(); err != nil {
118| 			slog.Error("Consumption error", ...)
119| 			c.markDisconnected()
120| 		}
121| 	}
122| }
```

*   **Line 117**: 如果已连接，调用 `consume()` 阻塞读取消息。
*   **Line 119**: **错误处理**。一旦消费出错（例如 Socket 断开），立即标记为断开，下一次循环将自动进入重连逻辑。

## 4. 辅助函数 (createSocket) - 代码复用

静态版提取了公共的 Socket 创建逻辑。

```go
160| func (c *StaticZMQClient) createSocket(t zmq.Type, port int) (*zmq.Socket, error) {
161| 	sock, err := zmq.NewSocket(t)
...
166| 	_ = sock.SetIpv6(true)
167|
168| 	endpoint := fmt.Sprintf("tcp://%s:%d", c.config.PodIP, port)
169| 	if err := sock.Connect(endpoint); err != nil {
170| 		sock.Close()
171| 		return nil, fmt.Errorf(...)
172| 	}
173| 	return sock, nil
174| }
```

*   **Line 160**: 接收 ZMQ 类型（SUB/DEALER）和端口作为参数。
*   **Line 166**: **简化 IPv6**。直接忽略 `SetIpv6` 的返回值。在静态可控网络中，我们可以假设如果开启失败也不影响（或者环境根本不支持）。
*   **Line 170**: **资源安全**。如果 Connect 失败，确保关闭 Socket。

## 5. 消费逻辑 (consume)

```go
178| func (c *StaticZMQClient) consume() error {
...
187| 	polled, err := socket.Poll(zmq.POLLIN, c.config.PollTimeout)
...
190| 	if len(polled) == 0 {
191| 		return nil // No data, continue loop
192| 	}
```

*   **Line 187**: **IO 多路复用**。使用 `Poll` 等待数据，带有超时时间。
*   **Line 191**: 超时未收到数据返回 `nil`，主循环会再次调用 `consume`。这允许主循环有机会检查 `ctx.Done()`，从而响应停止信号。

---

## 总结与对比

| 特性 | 原版 (ZMQClient) | 静态版 (StaticZMQClient) |
| :--- | :--- | :--- |
| **重连策略** | 指数退避 (1s -> 2s -> 4s...) | 固定间隔 (1s) |
| **代码结构** | 多个 Goroutine 协作，状态分散 | 单一主循环，状态机清晰 |
| **IPv6 处理** | 严格检查，失败则报错 | 尽力开启，忽略错误 |
| **代码复用** | 重复的 Socket 创建代码 | 提取 `createSocket` 函数 |
| **数据补齐** | 检测到 Gap 仅报警 | 重连后自动请求重放 |

静态版通过牺牲对极端动态环境的适应性（如防止 API Server 过载），换取了**代码的可读性**和**逻辑的确定性**，非常适合您的固定服务数量场景。

