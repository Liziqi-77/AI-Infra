# Mooncake Conductor 文档

## 目录

1. [概述](#概述)
2. [架构设计](#架构设计)
3. [代码设计](#代码设计)
4. [对外接口](#对外接口)
5. [功能作用](#功能作用)
6. [使用方法](#使用方法)

---

## 概述

Mooncake Conductor 是一个分布式大语言模型（LLM）推理代理和调度系统，主要用于：

- **请求路由与负载均衡**：将客户端请求智能路由到后端推理节点
- **Prefill/Decode 分离**：支持将推理任务的前缀填充（Prefill）和解码（Decode）阶段分离到不同的节点执行
- **KV Cache 管理**：通过订阅 ZMQ 事件监控和管理分布式 KV Cache 状态
- **缓存感知路由**：基于前缀缓存命中情况优化节点选择

系统由两个核心组件组成：
- **conductor-proxy**：C++ 实现的 HTTP 代理服务
- **conductor-ctrl**：Go 实现的 KV Cache 事件控制器

---

## 架构设计


### 组件说明

#### 1. conductor-proxy


- 接收客户端 HTTP 请求（OpenAI 兼容 API）
- 分类请求（推理请求 vs 其他请求）
- 根据配置选择目标节点（混合节点、Prefill 节点、Decode 节点）
- 转发请求到后端节点
- 处理流式和非流式响应


#### 2. conductor-ctrl

- 订阅后端节点的 ZMQ 事件流
- 处理 KV Cache 的存储和删除事件
- 维护前缀缓存索引表
- 提供缓存命中查询 API



## 对外接口

### conductor-proxy HTTP API

#### 1. 推理请求接口

**POST /v1/chat/completions**

OpenAI 兼容的聊天补全接口。

**请求示例**：
```json
{
  "model": "qwen2.5-7b",
  "messages": [
    {"role": "user", "content": "Hello"}
  ],
  "stream": true,
  "max_tokens": 100
}
```

**响应**：
- 流式响应：Server-Sent Events (SSE) 格式
- 非流式响应：标准 JSON 格式

**POST /v1/completions**

OpenAI 兼容的文本补全接口。


#### 3. PD 分离相关请求体字段

当启用 PD 分离时，conductor-proxy 会在转发请求时添加：

```json
{
  "kv_transfer_params": {
    "do_remote_decode": true,
    "do_remote_prefill": false,
    "remote_engine_id": "...",
    "remote_block_ids": [...],
    "remote_host": "...",
    "remote_port": ...
  }
}
```

### conductor-ctrl HTTP API

#### 1. 缓存命中查询（规划中）

**POST /cache**

查询给定 token 序列的缓存命中情况。

**请求示例**：
```json
{
  "instances": ["127.0.0.1", "127.0.0.2"],
  "token_ids": [1, 2, 3, ...],
  "model_name": "qwen2.5-7b",
  "lora_id": -1
}
```

**响应**：
```json
{
  "best_prefiller": "127.0.0.1",
  "cache_hit_percent": 75,
  "matched_engines": {
    "127.0.0.1": 75,
    "127.0.0.2": 50
  }
}
```

#### 2. 健康检查

**GET /**

返回服务状态信息。

---

## 功能作用

### 1. 请求路由与负载均衡

conductor-proxy 根据节点能力将请求路由到合适的后端节点：

- **混合模式**：请求直接转发到支持完整推理的节点
- **PD 分离模式**：Prefill 和 Decode 分别路由到专用节点

当前使用简单的轮询（Round-Robin）算法，后续可扩展为基于负载的智能调度。

### 2. Prefill/Decode 分离

**优势**：
- **资源优化**：Prefill 节点专注于处理长序列，Decode 节点专注于生成
- **并行处理**：理论上可以并行执行多个请求的 Prefill 和 Decode 阶段
- **扩展性**：可以独立扩展 Prefill 和 Decode 节点

**工作流程**：
1. 接收客户端请求
2. 选择 Prefill 节点，发送修改后的请求（`max_tokens=1`, `stream=false`）
3. 从 Prefill 响应中提取 `kv_transfer_params`
4. 选择 Decode 节点，发送包含 `kv_transfer_params` 的请求
5. 将 Decode 节点的流式响应转发给客户端

### 3. KV Cache 事件监控

conductor-ctrl 通过 ZMQ 订阅后端节点的 KV Cache 事件：

- **BlockStoredEvent**：当新的 KV Cache 块被存储时触发
- **BlockRemovedEvent**：当 KV Cache 块被删除时触发

这些事件用于维护全局的前缀缓存索引。

### 4. 前缀缓存索引

**目的**：快速判断哪些引擎包含特定前缀的 KV Cache。

**数据结构**：
- 使用链式哈希将 token 序列映射为前缀哈希
- 维护 `prefixHash → [engineIP]` 的映射关系
- 按模型和 LoRA ID 隔离索引

**应用场景**：
- 缓存感知路由：选择缓存命中率最高的 Prefill 节点
- 缓存预热：了解哪些前缀已被缓存

### 5. 连接池管理

conductor-proxy 为每个后端节点维护独立的 HTTP 连接池：

- **非流式连接池**：用于一次性请求（如 Prefill 请求）
- **流式连接池**：用于流式响应（如 Decode 请求）

连接池配置：
- `max_connection`：每个节点的最大连接数
- `idle_timeout`：空闲连接超时时间
- `connect_retry_count`：连接重试次数

### 6. 错误处理与重试

- HTTP 请求失败时自动重试（可配置重试次数）
- ZMQ 连接断开时自动重连
- 支持事件重放（从上次序列号继续）

---

## 使用方法

### 编译

#### conductor-proxy

在 Mooncake 项目顶层目录执行：

```bash
# 安装依赖
bash [mooncake目录下的dependencies.sh]
bash [mooncake-conductor目录下的dependencies.sh]

# 编译
mkdir build
cd build
cmake ..
make -j
make install
```

编译完成后，`mooncake_conductor` 可执行文件将安装到 `/usr/local/bin`。

#### conductor-ctrl

```bash
cd conductor-ctrl
go build -o conductor-ctrl main.go
```

### 运行

#### conductor-proxy

**基本用法**（混合模式）：

```bash
mooncake_conductor \
  --port=8080 \
  --mixed_hosts="127.0.0.1" \
  --mixed_ports="8100"
```

**PD 分离模式**：

```bash
mooncake_conductor \
  --port=8080 \
  --prefiller_hosts="127.0.0.1,127.0.0.1" \
  --prefiller_ports="8001,8002" \
  --decoder_hosts="127.0.0.1" \
  --decoder_ports="8200"
```

**完整配置示例**：

```bash
mooncake_conductor \
  --port=8180 \
  --host="0.0.0.0" \
  --prefiller_hosts="127.0.0.1,10.0.0.1" \
  --prefiller_ports="8001,8002" \
  --decoder_hosts="127.0.0.1,10.0.0.2" \
  --decoder_ports="8200,8201" \
  --mixed_hosts="127.0.0.1" \
  --mixed_ports="8100" \
  --mooncake_store_host="10.175.119.75" \
  --mooncake_store_port=50098 \
  --pd_enable="true" \
  --max_retries=3 \
  --retry_delay=0.001
```

**命令行参数说明**：

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `--port` | int | 8000 | HTTP 服务器监听端口 |
| `--host` | string | localhost | HTTP 服务器监听地址 |
| `--prefiller_hosts` | string | "" | Prefill 节点主机列表（逗号分隔） |
| `--prefiller_ports` | string | "" | Prefill 节点端口列表（逗号分隔） |
| `--decoder_hosts` | string | "" | Decode 节点主机列表（逗号分隔） |
| `--decoder_ports` | string | "" | Decode 节点端口列表（逗号分隔） |
| `--mixed_hosts` | string | "" | 混合节点主机列表（逗号分隔） |
| `--mixed_ports` | string | "" | 混合节点端口列表（逗号分隔） |
| `--mooncake_store_host` | string | localhost | Mooncake Store 主机地址 |
| `--mooncake_store_port` | int | 50051 | Mooncake Store 端口 |
| `--pd_enable` | string | "" | 是否启用 PD 分离（"true"/"false"） |
| `--max_retries` | int | 3 | HTTP 请求最大重试次数 |
| `--retry_delay` | double | 0.001 | 重试延迟（秒） |

**环境变量**：

- `OPENAI_API_KEY`：用于设置请求的 Authorization 头

#### conductor-ctrl

**配置示例**（在代码中配置）：

```go
services := []common.ServiceConfig{
    {
        Name:      "vllm-local",
        IP:        "127.0.0.1",
        Port:      5557,  // ZMQ publisher port
        Type:      common.ServiceTypeVLLM,
        ModelName: "qwen2.5-7b",
        LoraID:    -1,
    },
}

indexer := prefixindex.NewPrefixCacheTable()
manager := kvevent.NewEventManager(services, indexer)

// 启动 HTTP 服务器（可选）
manager.StartHTTPServer("33333")

// 启动事件管理器
manager.Start()
```

**环境变量**：

- `GATEWAY_TYPE`：网关类型（默认 "None"）
- `CONDUCTOR_BLOCK_SIZE`：块大小（默认 128）
- `NONE_HASH`：空哈希值（默认 1234）

### 停止服务

- **conductor-proxy**：按 `Ctrl+C` 停止
- **conductor-ctrl**：按 `Ctrl+C` 或发送 `SIGTERM` 信号

### 故障排查

#### Python 动态库问题

如果运行 `mooncake_conductor` 时出现：

```
mooncake_conductor: error while loading shared libraries: libpython3.xx.so.x.x: cannot open shared object file: No such file or directory
```

**解决方案**：

1. 找到动态库路径：
```bash
find / -name "libpython3.xx.so.x.x"
```

2. 临时解决方案（当前终端有效）：
```bash
export LD_LIBRARY_PATH=[动态库文件所在目录]:$LD_LIBRARY_PATH
```

3. 永久解决方案：
```bash
# 方法1：写入配置文件
echo "[动态库文件所在目录]" | sudo tee -a /etc/ld.so.conf.d/python3.conf
sudo ldconfig

# 方法2：创建软链接
sudo ln -s [动态库文件路径] /usr/lib/
sudo ldconfig
```

#### ZMQ 连接问题

如果 conductor-ctrl 无法连接到后端节点：

1. 检查后端节点的 ZMQ Publisher 是否运行
2. 检查防火墙设置
3. 查看日志中的连接错误信息

### 示例代码

参考 `example/cacheaware_disaggregated_proxy.py` 了解如何集成 conductor-ctrl 的缓存查询 API。

---

## 总结

Mooncake Conductor 提供了一个完整的分布式 LLM 推理代理和调度解决方案，支持：

- ✅ 灵活的节点能力配置（Prefill/Decode/Mixed）
- ✅ Prefill/Decode 分离架构
- ✅ 实时 KV Cache 事件监控
- ✅ 前缀缓存索引维护
- ✅ 高并发协程异步处理
- ✅ 连接池优化
- ✅ 自动重连和错误恢复

系统设计注重可扩展性和性能，为大规模 LLM 推理服务提供了坚实的基础。

