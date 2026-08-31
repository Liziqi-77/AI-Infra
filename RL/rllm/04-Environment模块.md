# Environment 模块详解

Environment（环境）是强化学习中的核心概念之一。在 rLLM 中，Environment 负责：
1. 提供任务/问题给 Agent
2. 接收 Agent 的动作
3. 计算奖励
4. 返回观察结果和终止信号

本文档详细介绍 Environment 模块的设计、核心类和具体实现。

## 模块结构

```
rllm/environments/
├── __init__.py                  # 导出 BaseEnv, SingleTurnEnvironment, ToolEnvironment
├── env_utils.py                 # 环境工具函数（奖励计算等）
├── base/                        # 基础环境类
│   ├── base_env.py              # BaseEnv 抽象基类
│   ├── multi_turn_env.py        # MultiTurnEnvironment 多轮环境
│   └── single_turn_env.py       # SingleTurnEnvironment 单轮环境
├── tools/                       # 工具环境
│   ├── tool_env.py              # ToolEnvironment 工具执行环境
│   └── mcp_env.py               # MCP 环境
├── frozenlake/                  # 冰湖环境
├── browsergym/                  # BrowserGym 环境
├── swe/                         # SWE-bench 环境
├── code/                        # 代码竞赛环境
└── appworld/                    # AppWorld 环境
```

---

## 1. BaseEnv 抽象基类

`BaseEnv` 是所有环境的抽象基类，定义了类似 OpenAI Gym 的标准接口。

### 1.1 类定义

```python
class BaseEnv(ABC):
    """所有环境的抽象基类"""
    
    @property
    def idx(self) -> Any:
        """环境的索引或标识符，通常用于批处理"""
        return getattr(self, "_idx", None)
    
    @idx.setter
    def idx(self, value: Any):
        """设置环境的索引或标识符"""
        self._idx = value
    
    @abstractmethod
    def reset(self) -> tuple[dict, dict]:
        """重置环境到初始状态"""
        pass
    
    @abstractmethod
    def step(self, action: Any) -> tuple[Any, float, bool, dict]:
        """执行一步动作"""
        pass
    
    def close(self):
        """执行必要的清理工作"""
        return
    
    @staticmethod
    @abstractmethod
    def from_dict(info: dict) -> "BaseEnv":
        """从字典创建环境实例"""
        raise NotImplementedError
    
    @staticmethod
    def is_multithread_safe() -> bool:
        """检查环境是否线程安全"""
        return True
```

### 1.2 核心方法详解

#### `reset() -> tuple[dict, dict]`

**作用**：重置环境到初始状态，准备新的 Episode。

**返回值**：
- 第一个元素：初始观察（observation），通常包含任务信息
- 第二个元素：辅助信息（info），通常为空字典 `{}`

```python
def reset(self) -> tuple[dict, dict]:
    self.step_count = 0
    return self.task, {}  # (observation, info)
```

#### `step(action) -> tuple[Any, float, bool, dict]`

**作用**：在环境中执行一步动作。

**参数**：
- `action`：Agent 采取的动作，可以是字符串、字典或列表

**返回值**（遵循 Gym 风格）：
- `observation`：执行动作后的新观察
- `reward`：获得的奖励（浮点数）
- `done`：Episode 是否结束（布尔值）
- `info`：额外的元数据信息

```python
def step(self, action):
    # 执行动作
    observation = ...
    reward = ...
    done = ...
    info = {"response": action, "metadata": {}}
    return observation, reward, done, info
```

#### `from_dict(info: dict) -> BaseEnv`

**作用**：从字典配置创建环境实例的工厂方法。

**用途**：在分布式训练中，需要将环境配置序列化为字典传递，然后在远端反序列化创建环境。

```python
@staticmethod
def from_dict(env_args: dict) -> "SingleTurnEnvironment":
    reward_fn = env_args.pop("reward_fn", None)
    task = env_args.get("task", env_args)
    return SingleTurnEnvironment(task=task, reward_fn=reward_fn)
```

#### `is_multithread_safe() -> bool`

**作用**：检查环境是否线程安全。

**用途**：在并行训练中，如果环境不是线程安全的，需要为每个工作线程创建独立的环境实例。

---

## 2. MultiTurnEnvironment - 多轮环境

`MultiTurnEnvironment` 是支持多轮交互的环境基类。

### 2.1 类定义

```python
class MultiTurnEnvironment(BaseEnv, ABC):
    """多轮交互环境基类"""
    
    def __init__(
        self,
        task: dict | None = None,    # 任务信息
        max_turns: int = 3,          # 最大轮数
        **kwargs
    ):
        super().__init__()
        self.task = task
        self.max_turns = max_turns
        self.current_turn = 0        # 当前轮数
        self.done = False            # 是否结束
        self.history = []            # 交互历史
```

### 2.2 工作流程

```
reset() → 设置 task，重置计数器
   │
   ▼
step(action) → 记录动作，计算奖励，检查是否达到 max_turns
   │
   ├── 如果 current_turn >= max_turns → done = True
   │
   └── 否则 → 返回下一个观察，继续交互
```

### 2.3 核心方法

#### `reset(task=None)`

```python
def reset(self, task: dict | None = None):
    if task is not None:
        self.task = task
    
    self.done = False
    self.current_turn = 0
    self.history = []
    
    return self.task, {}  # 返回初始观察
```

#### `step(action)`

```python
def step(self, action):
    # 记录动作
    self.history.append(action)
    
    # 计算奖励和下一个观察
    assert self.task is not None
    reward, next_obs = self.get_reward_and_next_obs(self.task, action)
    
    # 增加轮数
    self.current_turn += 1
    
    # 检查是否达到最大轮数
    if self.current_turn >= self.max_turns:
        self.done = True
        return {}, reward, self.done, self.task
    
    return next_obs, reward, self.done, self.task
```

#### `get_reward_and_next_obs(task, action) -> tuple[float, dict]`

**抽象方法**：子类必须实现此方法来计算奖励和下一个观察。

```python
@abstractmethod
def get_reward_and_next_obs(self, task: dict, action: Any) -> tuple[float, dict]:
    """
    计算奖励和下一个观察
    
    Args:
        task: 任务字典
        action: Agent 的动作
    
    Returns:
        (reward, next_observation) 元组
    """
    pass
```

### 2.4 自定义多轮环境示例

```python
class ChatEnvironment(MultiTurnEnvironment):
    def __init__(self, task=None, max_turns=5):
        super().__init__(task=task, max_turns=max_turns)
    
    def get_reward_and_next_obs(self, task, action):
        # 简单的奖励逻辑：如果回答包含关键词则给奖励
        keyword = task.get("keyword", "")
        if keyword.lower() in action.lower():
            reward = 1.0
        else:
            reward = 0.0
        
        # 返回下一个观察（可以是提示或上下文）
        next_obs = {"context": f"用户说: {action}"}
        
        return reward, next_obs
```

---

## 3. SingleTurnEnvironment - 单轮环境

`SingleTurnEnvironment` 是 `MultiTurnEnvironment` 的特例，`max_turns=1`，适用于只需一次回答就能完成的任务。

### 3.1 类定义

```python
class SingleTurnEnvironment(MultiTurnEnvironment):
    """单轮交互环境"""
    
    def __init__(
        self,
        task: dict | None = None,
        reward_fn: RewardFunction | None = None,  # 奖励函数
        **kwargs
    ):
        super().__init__(task=task, max_turns=1, **kwargs)
        self.reward_fn = reward_fn or zero_reward
```

### 3.2 核心方法

#### `get_reward_and_next_obs(task, action)`

```python
def get_reward_and_next_obs(self, task: dict, action: Any) -> tuple[float, dict]:
    # 调用奖励函数计算奖励
    reward_output = self.reward_fn(task_info=task, action=action)
    return reward_output.reward, {}  # 没有下一个观察
```

### 3.3 使用示例

```python
from rllm.rewards import math_reward_fn
from rllm.environments import SingleTurnEnvironment

# 创建单轮环境
env = SingleTurnEnvironment(
    task={
        "question": "1+1=?",
        "ground_truth": "2",
    },
    reward_fn=math_reward_fn,
)

# 重置环境
observation, info = env.reset()
print(observation)  # {"question": "1+1=?", "ground_truth": "2"}

# 执行一步
action = "答案是 2"
obs, reward, done, info = env.step(action)
print(reward)  # 1.0（如果答案正确）
print(done)    # True（单轮环境总是立即结束）
```

---

## 4. ToolEnvironment - 工具执行环境

`ToolEnvironment` 是一个专门用于工具型 Agent 的环境，负责执行工具调用并计算奖励。

### 4.1 类定义

```python
class ToolEnvironment(BaseEnv):
    """工具执行环境"""
    
    def __init__(
        self,
        task: dict | None = None,
        tools: list[str] | None = None,         # 工具名称列表
        tool_map: dict[str, type[Tool]] | None = None,  # 工具映射
        reward_fn: RewardFunction | None = None,
        max_steps: int = 10,                    # 最大步数
    ):
        self.step_count = 0
        self.max_steps = max_steps
        self.tools = MultiTool(tool_map=tool_map or {})
        self.task = task
        self.reward_fn = reward_fn or zero_reward
```

### 4.2 工作流程

```
reset() → 返回 task 作为初始观察
   │
   ▼
step(action) → 检查动作类型
   │
   ├── 如果是字符串 → 视为最终答案，计算奖励，done=True
   │
   ├── 如果包含 "finish" 工具调用 → 提取答案，计算奖励，done=True
   │
   └── 否则 → 执行工具调用，返回工具输出，done=False
```

### 4.3 核心方法

#### `step(action)`

```python
def step(self, action: list[dict] | str | dict):
    if action is None:
        action = []
    
    if isinstance(action, dict):
        action = [action]
    
    self.step_count += 1
    
    # 检查是否应该终止
    done = self.step_count >= self.max_steps or isinstance(action, str)
    
    # 检查是否包含 "finish" 工具调用
    if isinstance(action, list) and action:
        for tool_call in action:
            if tool_call.get("function", {}).get("name") == "finish":
                done = True
                break
    
    if done:
        # 提取最终答案
        if isinstance(action, str):
            llm_response = action
        elif isinstance(action, list):
            # 查找 finish 工具调用
            finish_action = None
            for tool_call in action:
                if tool_call.get("function", {}).get("name") == "finish":
                    finish_action = tool_call
                    break
            if finish_action:
                arguments = finish_action.get("function", {}).get("arguments", {})
                llm_response = arguments.get("response", "")
            else:
                llm_response = str(action)
        
        # 计算奖励
        task_info = self.task if self.task is not None else {}
        reward_output = self.reward_fn(task_info=task_info, action=llm_response)
        
        return {}, reward_output.reward, done, {
            "response": action,
            "metadata": reward_output.metadata,
            "is_correct": reward_output.is_correct,
        }
    
    # 执行工具调用
    tool_calls = action
    tool_outputs = self._execute_tool_calls(tool_calls)
    next_obs = {"tool_outputs": tool_outputs}
    
    return next_obs, 0, done, {"response": action, "metadata": {}}
```

#### `_execute_tool_calls(tool_calls)`

**作用**：并行执行多个工具调用（使用线程）。

```python
def _execute_tool_calls(self, tool_calls: list[dict]) -> dict[str, str]:
    import threading
    
    tool_outputs = {}
    output_queue = queue.Queue()
    threads = []
    
    def execute_tool(tool_call):
        tool_name = tool_call["function"]["name"]
        tool_args = json.loads(tool_call["function"]["arguments"])
        tool_output = self.tools(tool_name=tool_name, **tool_args)
        output_queue.put((tool_call["id"], tool_output.to_string()))
    
    # 创建并启动线程
    for tool_call in tool_calls:
        thread = threading.Thread(target=execute_tool, args=(tool_call,))
        threads.append(thread)
        thread.start()
    
    # 等待所有线程完成
    for thread in threads:
        thread.join()
    
    # 收集结果
    while not output_queue.empty():
        tool_call_id, output_str = output_queue.get()
        tool_outputs[tool_call_id] = output_str
    
    return tool_outputs
```

### 4.4 使用示例

```python
from rllm.environments import ToolEnvironment
from rllm.rewards import code_reward_fn

# 创建工具环境
env = ToolEnvironment(
    task={
        "question": "写一个函数计算两个数的和",
        "test_cases": [...],
    },
    tool_map={"python_interpreter": PythonInterpreter},
    reward_fn=code_reward_fn,
    max_steps=10,
)

# 重置
observation, info = env.reset()

# Agent 调用工具
action = [{
    "id": "call_1",
    "type": "function",
    "function": {
        "name": "python_interpreter",
        "arguments": '{"code": "def add(a, b): return a + b"}'
    }
}]
obs, reward, done, info = env.step(action)
print(obs)  # {"tool_outputs": {"call_1": "执行结果..."}}
print(done)  # False

# Agent 提交答案
action = [{
    "id": "call_2",
    "type": "function",
    "function": {
        "name": "finish",
        "arguments": '{"response": "def add(a, b): return a + b"}'
    }
}]
obs, reward, done, info = env.step(action)
print(reward)  # 根据代码执行结果计算
print(done)    # True
```

---

## 5. 其他环境实现

### 5.1 FrozenLake 环境

用于经典的 FrozenLake（冰湖）网格世界。Agent 需要在冰面上移动，避开洞到达目标。

### 5.2 BrowserGym 环境

用于浏览器自动化任务的环境，Agent 可以与网页进行交互。

### 5.3 SWE 环境

用于 SWE-bench 软件工程任务的环境，Agent 可以操作 GitHub 仓库。

### 5.4 Code 环境

用于代码竞赛任务的环境，通常与代码执行和测试配合使用。

### 5.5 AppWorld 环境

用于 AppWorld 基准测试的环境，Agent 可以与各种应用程序交互。

---

## 6. 环境与 Agent 的交互流程

```
┌─────────────┐                          ┌─────────────┐
│ Environment │                          │    Agent    │
│             │                          │             │
│  reset()    │──── observation ────────▶│             │
│             │                          │  生成动作    │
│             │◀────── action ───────────│             │
│  step()     │                          │             │
│             │──── obs, reward, done ──▶│             │
│             │                          │  更新状态    │
│             │                          │             │
│  如果 done  │                          │  结束       │
│  否则继续   │◀──── 继续交互 ───────────│             │
└─────────────┘                          └─────────────┘
```

**完整流程**：

1. **初始化**：创建环境和 Agent 实例
2. **重置**：调用 `env.reset()` 获取初始观察
3. **Agent 接收观察**：调用 `agent.update_from_env(observation, ...)`
4. **模型生成**：Engine 根据 `agent.chat_completions` 生成响应
5. **Agent 处理响应**：调用 `agent.update_from_model(response)` 获取动作
6. **环境执行动作**：调用 `env.step(action)` 获取新的观察、奖励、是否结束
7. **重复 3-6**：直到 `done=True`
8. **收集轨迹**：通过 `agent.trajectory` 获取完整轨迹

---

## 7. 如何自定义环境

### 7.1 自定义单轮环境

```python
from rllm.environments import SingleTurnEnvironment
from rllm.rewards import RewardFunction

class MySingleTurnEnv(SingleTurnEnvironment):
    def __init__(self, task=None, reward_fn=None):
        super().__init__(task=task, reward_fn=reward_fn)
    
    @staticmethod
    def from_dict(env_args: dict) -> "MySingleTurnEnv":
        reward_fn = env_args.pop("reward_fn", None)
        task = env_args.get("task", env_args)
        return MySingleTurnEnv(task=task, reward_fn=reward_fn)
```

### 7.2 自定义多轮环境

```python
from rllm.environments.base.multi_turn_env import MultiTurnEnvironment

class MyMultiTurnEnv(MultiTurnEnvironment):
    def __init__(self, task=None, max_turns=5):
        super().__init__(task=task, max_turns=max_turns)
    
    def get_reward_and_next_obs(self, task, action):
        # 自定义奖励逻辑
        reward = self._compute_reward(task, action)
        
        # 自定义下一个观察
        next_obs = {"hint": self._generate_hint(task, action)}
        
        return reward, next_obs
    
    def _compute_reward(self, task, action):
        # 实现你的奖励逻辑
        return 1.0 if "correct" in action.lower() else 0.0
    
    def _generate_hint(self, task, action):
        # 实现你的提示生成逻辑
        return f"试试另一种方法"
    
    @staticmethod
    def from_dict(env_args: dict) -> "MyMultiTurnEnv":
        task = env_args.get("task", env_args)
        max_turns = env_args.get("max_turns", 5)
        return MyMultiTurnEnv(task=task, max_turns=max_turns)
```

### 7.3 自定义工具环境

```python
from rllm.environments.tools.tool_env import ToolEnvironment

class MyToolEnv(ToolEnvironment):
    def __init__(self, task=None, tools=None, reward_fn=None, max_steps=10):
        super().__init__(
            task=task,
            tools=tools,
            reward_fn=reward_fn,
            max_steps=max_steps,
        )
    
    @staticmethod
    def from_dict(env_args: dict) -> "MyToolEnv":
        tools = env_args.pop("tools", None)
        reward_fn = env_args.pop("reward_fn", None)
        max_steps = env_args.pop("max_steps", 10)
        return MyToolEnv(task=env_args, tools=tools, reward_fn=reward_fn, max_steps=max_steps)
```

---

## 8. 环境在训练中的角色

在 rLLM 的训练流程中，环境扮演以下角色：

```
┌─────────────────────────────────────────────────────────┐
│                    Training Loop                         │
│                                                          │
│  ┌──────────┐    ┌──────────┐    ┌──────────┐           │
│  │          │    │          │    │          │           │
│  │ Env      │───▶│ Agent    │───▶│ Engine   │           │
│  │ reset()  │    │ update   │    │ generate │           │
│  │          │◀───│ from_env │◀───│          │           │
│  │          │    │          │    │          │           │
│  │ step()   │───▶│ update   │    │          │           │
│  │          │    │ from_model│   │          │           │
│  └──────────┘    └──────────┘    └──────────┘           │
│       │                                                  │
│       ▼                                                  │
│  ┌──────────┐                                           │
│  │ Reward   │                                           │
│  │ Function │                                           │
│  └──────────┘                                           │
└─────────────────────────────────────────────────────────┘
```

1. **提供任务**：环境通过 `reset()` 提供任务/问题
2. **执行动作**：环境通过 `step()` 执行 Agent 的动作
3. **计算奖励**：环境内部的奖励函数评估 Agent 的表现
4. **控制流程**：环境决定 Episode 何时结束（`done` 标志）

---

## 9. 环境配置映射

在 `rllm/trainer/env_agent_mappings.py` 中定义了环境与字符串名称的映射：

```python
ENV_CLASS_MAPPING = {
    "single_turn": SingleTurnEnvironment,
    "tool": ToolEnvironment,
    "frozenlake": FrozenLakeEnv,
    # ... 更多映射
}
```

这使得用户可以通过字符串名称来指定使用哪个环境类。

---

## 10. 总结

| 类 | 用途 | 特点 |
|----|------|------|
| `BaseEnv` | 抽象基类 | 定义标准 Gym 接口 |
| `MultiTurnEnvironment` | 多轮环境基类 | 支持多轮交互，可自定义奖励 |
| `SingleTurnEnvironment` | 单轮环境 | `max_turns=1`，适合问答任务 |
| `ToolEnvironment` | 工具执行环境 | 支持工具调用，并行执行 |
| `MCPEnv` | MCP 环境 | 支持 Model Context Protocol |

理解 Environment 模块后，建议继续学习 Workflow 模块，了解如何编排 Agent 和环境的交互。
