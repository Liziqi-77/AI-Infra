# consume (静态版) vs consumeEvents (原版) 深度对比

本文档详细对比了 `pkg/cache/kvcache/static_zmq_client.go` 中的 `consume` 方法与原版 `pkg/cache/kvcache/zmq_client.go` 中的 `consumeEvents` 方法。

---

## 1. 代码结构对比

### 原版 `consumeEvents` (Loop inside Method)

原版方法是一个**自包含的循环**。一旦被调用，它就会陷入死循环，直到 Context 取消。

```go
// 原版 zmq_client.go
func (c *ZMQClient) consumeEvents() error {
    // ... 获取 Socket ...
    
    poller := zmq.NewPoller() // 创建 Poller
    poller.Add(socket, zmq.POLLIN)

    for { // <--- 内部死循环
        select {
        case <-c.ctx.Done():
            return nil
        default:
            polled, err := poller.Poll(timeout)
            // ... 错误处理 ...
            if len(polled) == 0 {
                continue // 继续循环
            }
            if err := c.processMessage(); err != nil {
                return err // 返回错误，中断循环
            }
        }
    }
}
```

### 静态版 `consume` (Single Pass)

静态版方法执行**单次操作**。它只负责“尝试读取一次数据”，然后立即返回。循环逻辑被上移到了 `loop()` 主控函数中。

```go
// 静态版 static_zmq_client.go
func (c *StaticZMQClient) consume() error {
    // ... 获取 Socket ...

    // 直接使用 Socket 的 Poll 方法，不创建 Poller 对象
    polled, err := socket.Poll(zmq.POLLIN, c.config.PollTimeout)
    if err != nil {
        return fmt.Errorf(...)
    }
    if len(polled) == 0 {
        return nil // 无数据，直接返回 nil
    }

    // ... 读取并处理消息 ...
    // ... 成功后返回 nil
}
```

---

## 2. 核心区别解析

### 区别一：控制流反转 (Inversion of Control)

*   **原版**：`consumeEvents` 掌管控制流。主循环 (`consumeEventsWithReconnect`) 只是简单地调用它，一旦出错才退出来处理重连。
*   **静态版**：`consume` 只是一个无状态的执行单元。主循环 (`loop`) 完全掌管控制流（连接状态检查、重连定时器、退出信号）。

**修改原因**：
静态版的 `loop` 函数集成了一切逻辑（重连、重放、消费）。如果 `consume` 内部还有死循环，会让 `loop` 的逻辑变得割裂且难以理解。将 `consume` 改为单次执行，使得 `loop` 代码读起来像一个清晰的状态机：
> "检查连接 -> 没连上就重连 -> 连上了就消费一次 -> 循环"

### 区别二：Poller 的使用

*   **原版**：显式创建 `zmq.NewPoller()` 并添加 Socket。这是因为原版可能预留了监听多个 Socket 的能力（虽然目前只监听了一个）。
*   **静态版**：直接调用 `socket.Poll()`。

**修改原因**：
我们在静态场景下只需要监听一个 SUB Socket。直接调用 `socket.Poll` 代码更少，且没有创建 `Poller` 对象的开销。

### 区别三：消息处理逻辑内联

*   **原版**：调用 `c.processMessage()` 来读取和解析消息。
*   **静态版**：将 `processMessage` 的逻辑（RecvBytes, Decode, Handle）**内联**到了 `consume` 中。

**修改原因**：
1.  **减少函数调用栈**：在高性能要求的 I/O 循环中，减少一层函数调用微不足道但有益。
2.  **逻辑紧凑**：在静态版中，我们移除了很多防御性代码。处理逻辑变短了，没必要再拆分成两个函数。现在打开 `consume` 函数，能一眼看完“轮询 -> 读取 -> 解析 -> 回调”的全过程。

### 区别四：错误处理

*   **原版**：遇到解码错误 (`DecodeEventBatch`) 只是记录日志并继续（在 `processMessage` 内部吞掉了错误），只有 Socket 错误才会返回。
*   **静态版**：遇到任何错误（包括解码错误）都会返回 `error`。

**修改原因**：
静态版由 `loop` 统一处理错误。如果发生解码错误，虽然不需要断开连接，但返回错误让 `loop` 记录日志并决定下一步（目前也是记录日志），保持了“错误上报”的原则，逻辑更清晰。

---

## 3. 总结

| 特性 | 原版 (`consumeEvents`) | 静态版 (`consume`) | 评价 |
| :--- | :--- | :--- | :--- |
| **执行模式** | 内部死循环 | 单次执行，立即返回 | 静态版更利于上层编排 |
| **Poller** | `zmq.NewPoller()` | `socket.Poll()` | 静态版更轻量 |
| **结构** | 调用 `processMessage` | 逻辑内联 | 静态版更紧凑 |
| **设计哲学** | 封装完整职责 | 提供原子能力 | 静态版符合“组合优于继承” |

这种修改是为了配合 `StaticZMQClient` 的**单一主循环 (`loop`)** 设计模式，使得代码从“多层嵌套循环”变成了“扁平化单一循环”，极大地降低了认知负荷和维护难度。

