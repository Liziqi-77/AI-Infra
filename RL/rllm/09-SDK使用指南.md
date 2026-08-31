# SDK 使用指南

rLLM 的 SDK（Software Development Kit）提供了一套工具，用于**自动收集 LLM 调用信息**，无需修改你的 Agent 代码。这是 rLLM 最强大的特性之一。

## 模块结构

```
rllm/sdk/
├── __init__.py                  # 导出 trajectory 装饰器、Session、Tracer
├── protocol.py                  # Trace, LLMInput, LLMOutput, trace_to_step
├── decorators.py                # @trajectory 装饰器
├── shortcuts.py                 # session(), get_chat_client() 等快捷函数
├── config.yaml                  # SDK 配置
├── data_process.py              # 数据处理工具
├── chat/                        # 追踪的聊天客户端
│   ├── proxy_tracked.py         # 代理追踪客户端
│   └── otel_tracked.py          # OTel 追踪客户端
├── session/                     # 会话管理
│   ├── base.py                  # SessionProtocol
│   ├── contextvar.py            # ContextVarSession（默认后端）
│   ├── opentelemetry.py         # OpenTelemetrySession
│   ├── session_buffer.py        # SessionBuffer
│   └── storage.py               # 存储后端
├── tracers/                     # Tracer 实现
│   ├── base.py                  # TracerProtocol
│   ├── memory.py                # InMemorySessionTracer
│   └── sqlite.py                # SqliteTracer
└── integrations/                # 第三方框架集成
    ├── adk/                     # Google ADK
    ├── openai_agents/           # OpenAI Agents SDK
    └── strands/                 # Strands Agents SDK
```

---

## 1. 核心概念

### 1.1 Trace（追踪）

`Trace` 表示一次 LLM 调用的完整记录：

```python
@dataclass
class Trace:
    input: LLMInput       # 输入（消息、模型参数等）
    output: LLMOutput     # 输出（响应、token IDs、logprobs 等）
    metadata: dict        # 元数据
    latency: float        # 延迟
    tokens: int           # token 数量
```

### 1.2 Session（会话）

`Session` 是一组相关的 LLM 调用。在 Session 上下文中的所有 LLM 调用都会被自动追踪。

### 1.3 工作流程

```
┌─────────────┐
│   session() │◀── 创建会话上下文
│             │
│  你的代码    │
│  llm.create │◀── LLM 调用被自动拦截
│  llm.create │◀── 再次调用，也被拦截
│             │
│  结束       │◀── 会话结束，收集所有 traces
└─────────────┘
```

---

## 2. 快速开始

### 2.1 基本用法

```python
import rllm

# 获取追踪的聊天客户端
llm = rllm.get_chat_client(api_key="sk-...", base_url="https://api.openai.com/v1")

# 在 session 上下文中使用
with rllm.session(experiment="v1"):
    response = llm.chat.completions.create(
        model="gpt-4",
        messages=[{"role": "user", "content": "你好"}],
    )
    # 这次调用被自动追踪！
```

### 2.2 使用 @trajectory 装饰器

```python
from rllm.sdk import trajectory
from rllm.types import Trajectory

@trajectory(name="solver")
async def solve_problem(problem: str):
    llm = rllm.get_chat_client_async(api_key="sk-...")
    
    # 第一次 LLM 调用
    response1 = await llm.chat.completions.create(
        model="gpt-4",
        messages=[{"role": "user", "content": problem}],
    )
    
    # 第二次 LLM 调用
    response2 = await llm.chat.completions.create(
        model="gpt-4",
        messages=[
            {"role": "user", "content": problem},
            {"role": "assistant", "content": response1.choices[0].message.content},
            {"role": "user", "content": "请详细解释"},
        ],
    )
    
    return response2.choices[0].message.content

# 调用函数
traj: Trajectory = await solve_problem("1+1=?")

# 查看结果
print(f"轨迹名称: {traj.name}")          # "solver"
print(f"步骤数量: {len(traj.steps)}")    # 2
print(f"输入参数: {traj.input}")         # {"problem": "1+1=?"}
print(f"输出结果: {traj.output}")        # 函数返回值

# 设置奖励
traj.steps[0].reward = 0.5
traj.steps[1].reward = 1.0
traj.reward = sum(s.reward for s in traj.steps)
```

---

## 3. 核心 API

### 3.1 session()

创建会话上下文，自动追踪其中的 LLM 调用：

```python
def session(**metadata: Any):
    """
    创建会话上下文
    
    Args:
        **metadata: 附加到该会话中所有 trace 的元数据
    
    Returns:
        SessionContext: 会话上下文管理器
    
    Example:
        >>> with session(experiment="v1", model="gpt-4"):
        ...     llm.chat.completions.create(...)  # traces 获得 metadata
    """
```

**嵌套会话**：

```python
with session(experiment="v1"):
    llm.chat.completions.create(...)  # 获得 experiment="v1"
    
    with session(task="math"):
        llm.chat.completions.create(...)  # 获得 experiment="v1" + task="math"
```

### 3.2 get_chat_client()

获取带有自动会话追踪的 OpenAI 聊天客户端：

```python
def get_chat_client(
    provider: str = "openai",
    *,
    use_proxy: bool = True,
    **kwargs: Any,
):
    """
    获取带有自动会话追踪的 OpenAI 聊天客户端
    
    Args:
        provider: 提供商名称（仅支持 "openai"）
        use_proxy: 启用代理功能（默认 True）
        **kwargs: 传递给 OpenAI 客户端的参数
    
    Returns:
        TrackedChatClient: 带有会话追踪的 OpenAI 客户端
    
    Example:
        >>> llm = get_chat_client(api_key="sk-...", base_url="...")
        >>> with session(experiment="v1"):
        ...     llm.chat.completions.create(model="gpt-4", messages=[...])
    """
```

### 3.3 get_chat_client_async()

异步版本：

```python
def get_chat_client_async(
    provider: str = "openai",
    *,
    use_proxy: bool = True,
    **kwargs: Any,
):
    """
    获取异步 OpenAI 聊天客户端
    
    Example:
        >>> llm = get_chat_client_async(api_key="sk-...")
        >>> with session(experiment="v1"):
        ...     await llm.chat.completions.create(...)
    """
```

### 3.4 @trajectory 装饰器

标记函数为轨迹，自动将函数内的所有 LLM 调用转换为 Step：

```python
def trajectory(name: str = "agent", **traj_metadata):
    """
    装饰器，标记函数为轨迹
    
    Args:
        name: 轨迹名称
        **traj_metadata: 轨迹的额外元数据
    
    Returns:
        装饰器，包装函数返回 Trajectory
    
    Example:
        >>> @trajectory(name="solver")
        >>> async def solve(problem: str):
        ...     response = await llm.create(messages=[...])
        ...     return response.content
        
        >>> traj = await solve("1+1=?")
        >>> print(len(traj.steps))  # 1
    """
```

**重要**：`@trajectory` 装饰器会**改变函数的返回值**，返回 `Trajectory` 而不是原始返回值。原始返回值存储在 `traj.output` 中。

---

## 4. 追踪的聊天客户端

### 4.1 ProxyTrackedChatClient

基于代理的追踪客户端，适用于 ContextVar 后端：

```python
class ProxyTrackedChatClient:
    def __init__(self, use_proxy=True, **kwargs):
        self.client = OpenAI(**kwargs)
        self.use_proxy = use_proxy
    
    @property
    def chat(self):
        return TrackedChatCompletions(self.client.chat, use_proxy=self.use_proxy)
```

### 4.2 OpenTelemetryTrackedChatClient

基于 OpenTelemetry 的追踪客户端：

```python
class OpenTelemetryTrackedChatClient:
    def __init__(self, use_proxy=True, **kwargs):
        self.client = OpenAI(**kwargs)
        self.use_proxy = use_proxy
```

---

## 5. Session 后端

### 5.1 ContextVarSession（默认）

使用 Python 的 ContextVar 来管理会话状态：

```python
class ContextVarSession:
    """基于 ContextVar 的会话"""
    
    def __init__(self):
        self._session_var = ContextVar("session", default=None)
    
    @property
    def current_session(self):
        return self._session_var.get()
```

### 5.2 OpenTelemetrySession

使用 OpenTelemetry 来管理会话：

```python
class OpenTelemetrySession:
    """基于 OpenTelemetry 的会话"""
    
    def __init__(self):
        # 使用 OTel 的 trace context
        ...
```

---

## 6. Tracer 实现

### 6.1 InMemorySessionTracer

内存中的追踪器：

```python
class InMemorySessionTracer:
    """内存中的会话追踪器"""
    
    def __init__(self):
        self.traces = []
    
    def add_trace(self, trace: Trace):
        self.traces.append(trace)
    
    def get_traces(self) -> list[Trace]:
        return self.traces
```

### 6.2 SqliteTracer

基于 SQLite 的持久化追踪器：

```python
class SqliteTracer:
    """基于 SQLite 的持久化追踪器"""
    
    def __init__(self, db_path: str):
        self.db_path = db_path
        self._init_db()
    
    def add_trace(self, trace: Trace):
        # 存储到 SQLite
        ...
    
    def get_traces(self) -> list[Trace]:
        # 从 SQLite 读取
        ...
```

---

## 7. 第三方框架集成

### 7.1 Google ADK

```python
from rllm.sdk.integrations.adk import setup_adk_tracking

setup_adk_tracking()
# 现在所有 ADK 的 LLM 调用都会被追踪
```

### 7.2 OpenAI Agents SDK

```python
from rllm.sdk.integrations.openai_agents import setup_openai_agents_tracking

setup_openai_agents_tracking()
# 现在所有 OpenAI Agents 的 LLM 调用都会被追踪
```

### 7.3 Strands Agents SDK

```python
from rllm.sdk.integrations.strands import setup_strands_tracking

setup_strands_tracking()
# 现在所有 Strands Agents 的 LLM 调用都会被追踪
```

---

## 8. 完整示例

### 8.1 单步任务

```python
import rllm
from rllm.sdk import trajectory
from rllm.types import Trajectory

@trajectory(name="solver")
async def solve(task: dict, config: dict) -> Trajectory:
    llm = rllm.get_chat_client_async(api_key="sk-...", base_url=config["base_url"])
    
    response = await llm.chat.completions.create(
        model=config["model"],
        messages=[{"role": "user", "content": task["question"]}],
    )
    
    return response.choices[0].message.content

# 使用
traj = await solve({"question": "1+1=?"}, {"model": "gpt-4", "base_url": "..."})
print(f"答案: {traj.output}")
print(f"步骤数: {len(traj.steps)}")
```

### 8.2 多步任务

```python
@trajectory(name="multi_step_solver")
async def multi_step_solve(task: dict, config: dict):
    llm = rllm.get_chat_client_async(api_key="sk-...")
    
    # 第一步：思考
    thinking = await llm.chat.completions.create(
        model=config["model"],
        messages=[{"role": "user", "content": f"请思考如何解决这个问题：{task['question']}"}],
    )
    
    # 第二步：解答
    answer = await llm.chat.completions.create(
        model=config["model"],
        messages=[
            {"role": "user", "content": task["question"]},
            {"role": "assistant", "content": thinking.choices[0].message.content},
            {"role": "user", "content": "请给出最终答案"},
        ],
    )
    
    return answer.choices[0].message.content

# 使用
traj = await multi_step_solve({"question": "计算 1+2+...+100"}, {"model": "gpt-4"})
print(f"步骤数: {len(traj.steps)}")  # 2
print(f"答案: {traj.output}")
```

### 8.3 与 Rollout/Evaluator 配合

```python
import rllm
from rllm.experimental.eval.types import Task, AgentConfig, EvalOutput

@rllm.rollout
def solve(task: Task, config: AgentConfig) -> Episode:
    client = rllm.get_chat_client(api_key="EMPTY", base_url=config.base_url)
    response = client.chat.completions.create(
        model=config.model,
        messages=[{"role": "user", "content": task.data["question"]}],
    )
    answer = response.choices[0].message.content or ""
    return Episode(
        trajectories=[Trajectory(name="solver", steps=[])],
        artifacts={"answer": answer},
    )

@rllm.evaluator
def score(task: dict, episode: Episode) -> EvalOutput:
    answer = episode.artifacts.get("answer", "")
    is_correct = answer.strip() == task["ground_truth"].strip()
    return EvalOutput(reward=1.0 if is_correct else 0.0, is_correct=is_correct)
```

---

## 9. SDK 配置

SDK 配置存储在 `rllm/sdk/config.yaml`：

```yaml
session_backend: "contextvar"  # 或 "opentelemetry"
```

---

## 10. 总结

| API | 用途 |
|-----|------|
| `session()` | 创建会话上下文，自动追踪 LLM 调用 |
| `get_chat_client()` | 获取带追踪的同步聊天客户端 |
| `get_chat_client_async()` | 获取带追踪的异步聊天客户端 |
| `@trajectory` | 装饰器，将函数内的 LLM 调用转换为 Trajectory |
| `Trace` | 单次 LLM 调用的记录 |
| `InMemorySessionTracer` | 内存追踪器 |
| `SqliteTracer` | SQLite 持久化追踪器 |

SDK 是 rLLM 的核心特性，它使得"一套代码，两种用途"成为可能：同一套 Agent 代码既可用于评估，也可用于训练。
