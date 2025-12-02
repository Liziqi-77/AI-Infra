# ZMQ Simple Examples - 学习指南

## 📚 示例文件说明

| 文件 | 说明 | 端口 |
|-----|------|-----|
| `zmq_simple_publisher.py` | 基础发布者 - 发送温度数据 | 5555 |
| `zmq_simple_subscriber.py` | 基础订阅者 - 接收所有消息 | 5555 |
| `zmq_topic_publisher.py` | 主题发布者 - 发送多种类型数据 | 5556 |
| `zmq_topic_subscriber.py` | 主题订阅者 - 可过滤特定主题 | 5556 |

---

## 🚀 快速开始

### 前置要求

```bash
# 安装 PyZMQ
pip install pyzmq
```

---

## 📖 示例1: 基础发布/订阅

### 运行步骤

**终端1 - 启动发布者:**
```bash
python zmq_simple_publisher.py
```

**终端2 - 启动订阅者:**
```bash
python zmq_simple_subscriber.py
```

### 预期输出

**发布者 (终端1):**
```
============================================================
ZMQ SIMPLE PUBLISHER
============================================================

[Step 1] Creating ZMQ context...
✓ Context created

[Step 2] Creating PUB socket...
✓ PUB socket created

[Step 3] Binding to tcp://*:5555...
✓ Publisher bound to tcp://*:5555
  (Listening on port 5555, accepting connections from subscribers)

[Step 4] Waiting 2 seconds for subscribers to connect...
✓ Ready to publish!

[Step 5] Publishing messages (press Ctrl+C to stop)...
------------------------------------------------------------
[14:23:01] Sent message #1: Temperature: 23°C
[14:23:02] Sent message #2: Temperature: 27°C
[14:23:03] Sent message #3: Temperature: 19°C
...
```

**订阅者 (终端2):**
```
============================================================
ZMQ SIMPLE SUBSCRIBER
============================================================

[Step 1] Creating ZMQ context...
✓ Context created

[Step 2] Creating SUB socket...
✓ SUB socket created

[Step 3] Connecting to publisher at tcp://localhost:5555...
✓ Connected to tcp://localhost:5555

[Step 4] Setting subscription filter...
✓ Subscribed to ALL topics (filter: '')
  (You can filter by topic, e.g., 'Temperature' to only receive those)

[Step 5] Waiting for messages (press Ctrl+C to stop)...
------------------------------------------------------------
[14:23:01] Received message #1: Temperature: 23°C
[14:23:02] Received message #2: Temperature: 27°C
[14:23:03] Received message #3: Temperature: 19°C
...
```

### 学习要点

1. **PUB socket 使用 bind()** - 发布者绑定到固定端口
2. **SUB socket 使用 connect()** - 订阅者连接到发布者
3. **必须设置订阅过滤器** - 即使是空字符串（订阅全部）
4. **启动顺序** - 先启动发布者，再启动订阅者

---

## 📖 示例2: 主题过滤

### 运行步骤

**终端1 - 启动主题发布者:**
```bash
python zmq_topic_publisher.py
```

**终端2 - 订阅所有主题:**
```bash
python zmq_topic_subscriber.py
```

**终端3 - 只订阅温度数据:**
```bash
python zmq_topic_subscriber.py Temperature
```

**终端4 - 只订阅湿度数据:**
```bash
python zmq_topic_subscriber.py Humidity
```

### 预期输出

**发布者 (终端1):**
```
============================================================
ZMQ TOPIC PUBLISHER
============================================================

✓ Publisher started on port 5556
  Publishing messages with 3 topics:
    - Temperature
    - Humidity
    - Pressure

Waiting 2 seconds for subscribers...
Starting to publish...

------------------------------------------------------------
[14:25:01] #  1 [Temperature  ] 25°C
[14:25:02] #  2 [Humidity     ] 65%
[14:25:03] #  3 [Pressure     ] 1013hPa
[14:25:04] #  4 [Temperature  ] 22°C
[14:25:05] #  5 [Humidity     ] 58%
...
```

**订阅者 - 全部 (终端2):**
```
✓ Connected to publisher on port 5556
✓ Subscribed to ALL topics

Waiting for messages...

------------------------------------------------------------
[14:25:01] #  1 Temperature 25°C
[14:25:02] #  2 Humidity 65%
[14:25:03] #  3 Pressure 1013hPa
[14:25:04] #  4 Temperature 22°C
[14:25:05] #  5 Humidity 58%
...
```

**订阅者 - 只要温度 (终端3):**
```
✓ Connected to publisher on port 5556
✓ Subscribed to topic filter: 'Temperature'
  (Only messages starting with 'Temperature' will be received)

Waiting for messages...

------------------------------------------------------------
[14:25:01] #  1 Temperature 25°C
[14:25:04] #  2 Temperature 22°C
[14:25:08] #  3 Temperature 28°C
...
```

**订阅者 - 只要湿度 (终端4):**
```
✓ Connected to publisher on port 5556
✓ Subscribed to topic filter: 'Humidity'
  (Only messages starting with 'Humidity' will be received)

Waiting for messages...

------------------------------------------------------------
[14:25:02] #  1 Humidity 65%
[14:25:05] #  2 Humidity 58%
[14:25:09] #  3 Humidity 72%
...
```

### 学习要点

1. **主题是消息的前缀** - ZMQ通过前缀匹配过滤消息
2. **一个发布者，多个订阅者** - 可以同时有多个订阅者
3. **每个订阅者独立过滤** - 不同订阅者可以订阅不同主题
4. **过滤在订阅者侧** - 发布者发送所有消息，订阅者决定接收哪些

---

## 🔬 实验和学习

### 实验1: 慢订阅者问题 (Slow Joiner)

```bash
# 1. 先启动发布者
python zmq_simple_publisher.py

# 2. 等待10秒后再启动订阅者
python zmq_simple_subscriber.py
```

**观察**: 订阅者不会收到连接前发布的消息 - 这就是"慢订阅者"问题！

### 实验2: 多个订阅者

```bash
# 终端1: 发布者
python zmq_simple_publisher.py

# 终端2-5: 同时启动4个订阅者
python zmq_simple_subscriber.py
python zmq_simple_subscriber.py
python zmq_simple_subscriber.py
python zmq_simple_subscriber.py
```

**观察**: 所有订阅者都会收到相同的消息 - 这是广播！

### 实验3: 订阅者断开重连

```bash
# 1. 启动发布者和订阅者
python zmq_simple_publisher.py   # 终端1
python zmq_simple_subscriber.py  # 终端2

# 2. 在终端2按 Ctrl+C 停止订阅者
# 3. 等待5秒
# 4. 重新启动订阅者
python zmq_simple_subscriber.py  # 终端2
```

**观察**: 订阅者只能收到重连后的消息，断开期间的消息丢失了！

### 实验4: 主题过滤的效率

```bash
# 修改 zmq_topic_subscriber.py，在接收循环中添加计数：
# 运行多个订阅者，观察不同过滤器的消息接收率
```

---

## 🧠 核心概念总结

### ZMQ PUB/SUB 模式特点

| 特点 | 说明 |
|-----|------|
| **一对多** | 一个发布者可以向多个订阅者广播 |
| **单向通信** | 发布者不知道订阅者的存在 |
| **Fire-and-forget** | 发布者不等待确认，立即发送 |
| **可能丢消息** | 连接前/断开时的消息会丢失 |
| **主题过滤** | 订阅者可以选择接收特定主题 |

### Socket 类型对比

| Socket | 动作 | 作用 |
|--------|------|------|
| **PUB** | bind() | 发布者，绑定固定地址 |
| **SUB** | connect() | 订阅者，连接到发布者 |

### 关键 API

```python
# 创建上下文
context = zmq.Context()

# 创建socket
socket = context.socket(zmq.PUB)  # 或 zmq.SUB

# 发布者: 绑定
socket.bind("tcp://*:5555")

# 订阅者: 连接
socket.connect("tcp://localhost:5555")

# 订阅者: 设置过滤器
socket.setsockopt_string(zmq.SUBSCRIBE, "topic")

# 发送消息
socket.send_string("message")

# 接收消息 (阻塞)
message = socket.recv_string()

# 清理
socket.close()
context.term()
```

---

## 🎯 下一步学习

1. **请求/响应模式** (REQ/REP) - 双向通信
2. **管道模式** (PUSH/PULL) - 任务分发
3. **路由模式** (ROUTER/DEALER) - 异步通信
4. **高水位标记** (HWM) - 流量控制
5. **序列号和重放** - 可靠性保障

---

## 🐛 常见问题

### Q1: 为什么订阅者收不到消息？

**可能原因:**
- 忘记设置订阅过滤器: `socket.setsockopt_string(zmq.SUBSCRIBE, "")`
- 订阅者启动太晚，错过了消息
- 端口号不匹配
- 防火墙阻止

### Q2: 为什么需要 `time.sleep(2)`？

ZMQ连接是异步的，需要时间建立。如果发布者立即发送消息，订阅者可能还未连接完成。

### Q3: 如何确保消息不丢失？

PUB/SUB模式本身不保证可靠性。要实现可靠性：
- 使用 REQ/REP 或 ROUTER/DEALER 模式
- 实现序列号和重放机制（见 vLLM 的实现）
- 使用持久化队列

### Q4: 可以跨机器运行吗？

可以！只需修改地址：
```python
# 发布者 (机器A, IP: 192.168.1.100)
socket.bind("tcp://*:5555")

# 订阅者 (机器B)
socket.connect("tcp://192.168.1.100:5555")
```

---

## 📝 练习题

1. **修改示例**: 让发布者发送 JSON 格式的数据
2. **添加功能**: 在订阅者侧计算接收速率（消息/秒）
3. **实现过滤**: 只显示温度 > 25°C 的消息
4. **错误处理**: 添加异常处理和重连逻辑
5. **多线程**: 一个程序同时运行发布者和订阅者

---

**Happy Learning! 🎉**

如有问题，请参考:
- [ZMQ Guide](https://zguide.zeromq.org/)
- [PyZMQ Documentation](https://pyzmq.readthedocs.io/)

