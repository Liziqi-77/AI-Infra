# Go ZMQ 学习指南

这份文档提供了使用 Go 语言编写 ZeroMQ 发布/订阅模式的完整指南，对应之前的 Python 学习示例。

## 目录
1. [前置要求](#1-前置要求)
2. [项目初始化](#2-项目初始化)
3. [代码文件说明](#3-代码文件说明)
4. [运行指南](#4-运行指南)
5. [Python vs Go 关键区别](#5-python-vs-go-关键区别)

---

## 1. 前置要求

Go 语言的 ZMQ 库 (`pebbe/zmq4`) 使用 CGO 调用底层的 ZeroMQ C 库，因此**必须**先安装系统级的 ZeroMQ 库。

### 安装 libzmq

- **Ubuntu / Debian:**
  ```bash
  sudo apt-get update
  sudo apt-get install libzmq3-dev
  ```

- **macOS (Homebrew):**
  ```bash
  brew install zeromq
  pkg-config --cflags --libs libzmq  # 验证安装
  ```

- **CentOS / RHEL:**
  ```bash
  sudo yum install zeromq-devel
  ```

- **Windows:**
  Windows 上配置 CGO 比较复杂。
  - **推荐**: 使用 **WSL (Windows Subsystem for Linux)**，在 WSL 中按照 Ubuntu 的步骤安装。
  - **替代**: 使用 MSYS2 安装 mingw-w64-x86_64-zeromq，并配置环境变量。

---

## 2. 项目初始化

在开始运行代码之前，需要初始化 Go 模块并下载依赖。

1. **创建项目目录**:
   ```bash
   mkdir go_zmq_demo
   cd go_zmq_demo
   ```

2. **初始化 Go Module**:
   ```bash
   go mod init zmq_demo
   ```

3. **下载 ZMQ 依赖**:
   ```bash
   go get github.com/pebbe/zmq4
   ```

---

## 3. 代码文件说明

| 文件名 | 对应 Python 文件 | 功能描述 | 端口 |
|--------|-----------------|----------|------|
| `zmq_simple_publisher.go` | `zmq_simple_publisher.py` | **基础发布者**<br>每秒发送一条温度数据。演示 `Bind` 和 `Send`。 | 5555 |
| `zmq_simple_subscriber.go` | `zmq_simple_subscriber.py` | **基础订阅者**<br>接收所有消息。演示 `Connect` 和 `Recv`。 | 5555 |
| `zmq_topic_publisher.go` | `zmq_topic_publisher.py` | **主题发布者**<br>随机发送 Temperature/Humidity/Pressure 数据。<br>演示多主题消息构造。 | 5556 |
| `zmq_topic_subscriber.go` | `zmq_topic_subscriber.py` | **主题订阅者**<br>支持命令行参数过滤主题。<br>演示 `SetSubscribe` 过滤功能。 | 5556 |

---

## 4. 运行指南

请打开两个终端窗口来进行交互测试。

### 场景 1: 基础发布/订阅 (端口 5555)

**终端 1 (发布者):**
```bash
go run zmq_simple_publisher.go
```

**终端 2 (订阅者):**
```bash
go run zmq_simple_subscriber.go
```

### 场景 2: 主题过滤 (端口 5556)

**终端 1 (发布者):**
```bash
go run zmq_topic_publisher.go
```

**终端 2 (订阅者 - 接收所有):**
```bash
go run zmq_topic_subscriber.go
```

**终端 3 (订阅者 - 只接收温度):**
```bash
go run zmq_topic_subscriber.go Temperature
```
*你会发现终端 3 只打印以 "Temperature" 开头的消息。*

---

## 5. Python vs Go 关键区别

从 Python 迁移到 Go 时，请注意以下 ZMQ 开发习惯的区别：

| 特性 | Python (`pyzmq`) | Go (`pebbe/zmq4`) | 说明 |
| :--- | :--- | :--- | :--- |
| **资源清理** | `try...finally`<br>`socket.close()` | `defer socket.Close()` | Go 的 `defer` 语句在资源创建后立即声明，确保函数退出时自动关闭，非常优雅且不易遗忘。 |
| **错误处理** | 异常机制 (`try...except`) | 显式检查 (`if err != nil`) | Go 中每一步操作（创建、绑定、发送、接收）都要检查错误。 |
| **Socket创建** | 需先创建 `Context`<br>`ctx.socket(zmq.PUB)` | `zmq.NewSocket(zmq.PUB)` | Go 库通常隐式处理全局 Context，简化了代码（也可以显式创建 Context）。 |
| **订阅设置** | `setsockopt_string(...)` | `SetSubscribe("...")` | Go 库封装了更友好的 API 方法名。 |
| **字符串格式化**| `f"Temp: {val}"` | `fmt.Sprintf("Temp: %d", val)` | Go 是强类型语言，字符串拼接需使用 `fmt` 包。 |
| **接收消息** | `recv_string()` | `Recv(0)` | `Recv` 返回字符串，参数 `0` 代表默认标志（阻塞模式）。 |

### 常见问题排查

**Q: 报错 `fatal error: zmq.h: No such file or directory`**
- **原因**: 找不到 C 头文件。
- **解决**: 确保已安装 `libzmq3-dev` (Linux) 或 `zeromq` (Mac)。如果已安装但仍报错，可能需要设置 `CGO_CFLAGS` 和 `CGO_LDFLAGS` 环境变量指向头文件和库文件的位置。

**Q: 报错 `imported and not used: "log"`**
- **原因**: Go 语言不允许导入包却不使用。
- **解决**: 检查代码，如果注释掉了使用 `log` 的行，也要删除对应的 import。

---

**Happy Coding with Go & ZMQ!** 🚀

