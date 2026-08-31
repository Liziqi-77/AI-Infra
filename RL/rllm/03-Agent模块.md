# Agent 模块详解

Agent 是 rLLM 框架中的核心概念之一，代表能够与环境交互、做出决策并执行动作的智能体。本文档详细介绍 Agent 模块的设计、核心类和具体实现。

## 模块结构

```
rllm/agents/
├── __init__.py              # 懒加载导入所有 Agent 类
├── agent.py                 # BaseAgent 抽象基类 + Step/Trajectory/Episode 扩展
├── math_agent.py            # MathAgent - 数学问题求解 Agent
├── tool_agent.py            # ToolAgent - 工具使用 Agent
├── code_agent.py            # CompetitionCodingAgent - 编程竞赛 Agent
├── frozenlake_agent.py      # FrozenLakeAgent - 冰湖游戏 Agent
├── miniwob_agent.py         # MiniWobAgent - 网页交互 Agent
├── swe_agent.py             # SWEAgent - 软件工程 Agent
├── webarena_agent.py        # WebArenaAgent - 网页基准测试 Agent
├── appworld_react_agents.py # AppWorld ReAct Agents
├── system_prompts.py        # Agent 专用的系统提示词
└── utils.py                 # Agent 工具函数
```

---

## 1. BaseAgent 抽象基类

`BaseAgent` 是所有 Agent 的抽象基类，定义了 Agent 必须实现的标准接口。

### 1.1 类定义

```python
class BaseAgent(ABC):
    """所有 Agent 的抽象基类"""
    
    @property
    def chat_completions(self) -> list[dict[str, str]]:
        """将 Agent 内部状态转换为 OpenAI 格式的对话列表"""
        return []
    
    @property
    def trajectory(self) -> Trajectory:
        """将 Agent 内部状态转换为 Trajectory 对象"""
        return Trajectory()
    
    def update_from_env(self, observation: Any, reward: float, done: bool, info: dict, **kwargs):
        """环境执行后更新 Agent 状态"""
        raise NotImplementedError
    
    def update_from_model(self, response: str, **kwargs) -> Action:
        """模型生成响应后更新 Agent 状态"""
        raise NotImplementedError
    
    @abstractmethod
    def reset(self):
        """重置 Agent 内部状态"""
        return
    
    def get_current_state(self) -> Step | None:
        """获取 Agent 当前状态（最后一步）"""
        if not self.trajectory.steps:
            return None
        return self.trajectory.steps[-1]
```

### 1.2 核心方法详解

#### `reset()`

**作用**：重置 Agent 的内部状态，通常在每个新 Episode 开始时调用。

**需要重置的内容**：
- 对话历史（`messages`）
- 轨迹记录（`_trajectory`）
- 其他内部状态变量

```python
def reset(self) -> None:
    """重置 Agent 状态"""
    self._trajectory = Trajectory()  # 清空轨迹
    self.messages = []               # 清空对话历史
```

#### `update_from_env(observation, reward, done, info)`

**作用**：接收环境的反馈并更新 Agent 状态。

**参数说明**：

| 参数 | 类型 | 说明 |
|------|------|------|
| `observation` | `Any` | 环境的观察结果，可以是字符串、字典等 |
| `reward` | `float` | 上一步动作获得的奖励 |
| `done` | `bool` | 当前 Episode 是否结束 |
| `info` | `dict` | 额外的元数据信息 |

**典型实现**：
```python
def update_from_env(self, observation, reward, done, info, **kwargs):
    # 如果有轨迹，更新最后一步的奖励和完成状态
    if self.trajectory.steps:
        cur_step = self.get_current_state()
        cur_step.reward = reward
        cur_step.done = done
        cur_step.info = info
    
    # 如果有新的观察，创建新的 Step
    if observation:
        self.messages.append({"role": "user", "content": str(observation)})
        new_step = Step(observation=observation)
        self._trajectory.steps.append(new_step)
```

#### `update_from_model(response, **kwargs) -> Action`

**作用**：接收模型的响应并更新 Agent 状态，返回要执行的动作。

**参数说明**：

| 参数 | 类型 | 说明 |
|------|------|------|
| `response` | `str` | 模型生成的文本响应 |
| `**kwargs` | - | 额外的关键字参数 |

**返回值**：`Action` 对象，包含 Agent 要执行的动作。

**典型实现**：
```python
def update_from_model(self, response: str, **kwargs) -> Action:
    # 将响应添加到对话历史
    self.messages.append({"role": "assistant", "content": response})
    
    # 解析响应（如提取思考过程和动作）
    thought, action_text = self._parse_response(response)
    
    # 更新当前 Step
    cur_step = self.get_current_state()
    cur_step.chat_completions = self.chat_completions
    cur_step.model_response = response
    cur_step.thought = thought
    cur_step.action = Action(action=action_text)
    
    return Action(action=action_text)
```

#### `chat_completions` 属性

**作用**：将 Agent 的内部对话历史转换为 OpenAI API 兼容的格式。

**返回格式**：
```python
[
    {"role": "system", "content": "你是一个助手"},
    {"role": "user", "content": "你好"},
    {"role": "assistant", "content": "你好！有什么可以帮助你的？"},
]
```

#### `trajectory` 属性

**作用**：将 Agent 的完整交互历史转换为 `Trajectory` 对象。

**返回内容**：包含所有 `Step` 的 `Trajectory` 对象。

#### `get_current_state() -> Step | None`

**作用**：获取 Agent 当前状态，即轨迹中的最后一个 Step。

**用途**：在更新状态时，用于获取当前正在处理的 Step。

---

## 2. MathAgent - 数学问题求解 Agent

`MathAgent` 是一个专门用于解决数学问题的 Agent，它支持逐步推理（Chain of Thought）。

### 2.1 类定义

```python
class MathAgent(BaseAgent):
    def __init__(self, accumulate_thinking=True):
        """
        Args:
            accumulate_thinking: 是否在对话历史中保留思考过程
                - True: 保留完整的 <think>...</think> 内容
                - False: 只保留最终答案，去除思考过程
        """
        self._trajectory = Trajectory()
        self.messages = []
        self.accumulate_thinking = accumulate_thinking
```

### 2.2 工作流程

```
1. reset() → 清空状态
2. update_from_env(question) → 接收问题，创建新 Step
3. update_from_model(response) → 解析模型响应，提取思考和答案
4. 重复 2-3 直到问题解决
5. trajectory → 获取完整轨迹用于训练
```

### 2.3 关键实现细节

**解析模型响应**：

MathAgent 假设模型使用 `<think>...</think>` 标签来分隔思考过程和最终答案：

```python
def update_from_model(self, response: str, **kwargs) -> Action:
    # 如果响应中包含 <think> 标签，分离思考和答案
    if response.count("</think>") == 1:
        thought, sep, action = response.partition("</think>")
        thought = thought + sep  # 包含 </think> 标签
        action = Action(action=action.strip())
    else:
        thought = None
        action = Action(action=response.strip())
    
    cur_step.thought = thought
    cur_step.action = action
    return action
```

**控制思考内容的保留**：

```python
@property
def chat_completions(self) -> list[dict[str, str]]:
    messages = copy.deepcopy(self.messages)
    if not self.accumulate_thinking:
        # 去除除最后一条外的所有 assistant 消息中的思考内容
        for msg in messages[:-1]:
            if msg["role"] == "assistant":
                _, sep, after = msg["content"].partition("</think>")
                if sep:
                    msg["content"] = after  # 只保留 </think> 之后的内容
    return messages
```

### 2.4 使用示例

```python
from rllm.agents import MathAgent

# 创建 Agent（保留思考过程）
agent = MathAgent(accumulate_thinking=True)

# 重置状态
agent.reset()

# 接收问题
agent.update_from_env(
    observation={"question": "计算 1+2+3+...+100 的和"},
    reward=0.0,
    done=False,
    info={},
)

# 获取对话历史发送给模型
messages = agent.chat_completions
# [{"role": "user", "content": "计算 1+2+3+...+100 的和"}]

# 假设模型返回响应
response = """<think>
这是一个等差数列求和问题。
首项 a1=1，末项 an=100，项数 n=100。
求和公式：S = n(a1+an)/2
</think>
S = 100 * (1 + 100) / 2 = 5050"""

# 更新模型响应
action = agent.update_from_model(response)
print(action.action)  # "S = 100 * (1 + 100) / 2 = 5050"

# 获取完整轨迹
trajectory = agent.trajectory
print(len(trajectory.steps))  # 1
print(trajectory.steps[0].thought)  # "<think>\n这是一个等差数列求和问题...\n</think>"
```

---

## 3. ToolAgent - 工具使用 Agent

`ToolAgent` 是一个能够使用外部工具（如代码执行、搜索等）与 environment 交互的 Agent。

### 3.1 类定义

```python
class ToolAgent(BaseAgent):
    def __init__(
        self,
        system_prompt=TOOL_SYSTEM_PROMPT,      # 系统提示词
        parser_name="qwen",                     # 工具调用解析器名称
        tools: list[str] | None = None,         # 工具名称列表（旧方式）
        tool_map: dict[str, type[Tool]] | None = None,  # 工具映射（新方式）
    ):
        self.system_prompt = system_prompt
        self.tools = MultiTool(tool_map=tool_map or {})  # 多工具管理器
        self.tool_parser = get_tool_parser(parser_name)()  # 工具调用解析器
        self.tools_prompt = self.tool_parser.get_tool_prompt(
            json.dumps(self.tools.json, indent=2)
        )
        self._trajectory = Trajectory()
        self.messages = []
        self.current_observation = None
        self.reset()
```

### 3.2 工具调用解析器

ToolAgent 使用 `ToolParser` 来从模型响应中解析工具调用。rLLM 内置了多种解析器：

| 解析器名称 | 类 | 适用模型 |
|-----------|-----|---------|
| `qwen` | `QwenToolParser` | Qwen 系列模型 |
| `r1` | `R1ToolParser` | R1 系列模型 |

**工具调用格式**：

模型响应中的工具调用会被解析为以下格式：

```python
[
    {
        "id": "uuid-1",
        "type": "function",
        "function": {
            "name": "python_interpreter",
            "arguments": '{"code": "print(1+1)"}'
        }
    }
]
```

### 3.3 工作流程

```
1. reset() → 初始化系统提示词 + 工具描述
2. update_from_env(question) → 接收问题
3. update_from_model(response) → 解析工具调用
4. 环境执行工具调用
5. update_from_env(tool_outputs) → 接收工具执行结果
6. 重复 3-5 直到完成
```

### 3.4 关键实现细节

**格式化观察为消息**：

```python
def _format_observation_as_messages(self, obs: Any) -> list[dict]:
    messages = []
    if isinstance(obs, dict):
        if "question" in obs:
            messages.append({"role": "user", "content": obs["question"]})
        elif "tool_outputs" in obs:
            # 格式化工具输出
            for tool_call_id, tool_output_str in obs["tool_outputs"].items():
                messages.append({
                    "role": "tool",
                    "content": tool_output_str,
                    "tool_call_id": tool_call_id,
                })
    elif isinstance(obs, str):
        messages.append({"role": "user", "content": obs})
    return messages
```

**解析工具调用**：

```python
def update_from_model(self, response: str, **kwargs) -> Action:
    tool_calls_dict = []
    
    # 尝试解析工具调用
    try:
        tool_calls = self.tool_parser.parse(response)
        tool_calls_dict = [
            {
                "id": str(uuid.uuid4()),
                "type": "function",
                "function": tool_call.to_dict(),
            }
            for tool_call in tool_calls
        ]
    except Exception as e:
        logger.error(f"Failed to parse tool calls: {e}")
    
    # 如果没有解析到工具调用，默认为 finish 动作
    if len(tool_calls_dict) == 0:
        tool_calls_dict = [{
            "id": str(uuid.uuid4()),
            "type": "function",
            "function": {
                "name": "finish",
                "arguments": {"response": response},
            }
        }]
    
    # 创建新的 Step
    new_step = Step(
        chat_completions=copy.deepcopy(self.chat_completions),
        action=tool_calls_dict,
        model_response=response,
        observation=self.current_observation,
    )
    self._trajectory.steps.append(new_step)
    
    return Action(action=tool_calls_dict)
```

### 3.5 MCPToolAgent

`MCPToolAgent` 是 `ToolAgent` 的子类，专门用于支持 MCP（Model Context Protocol）工具：

```python
class MCPToolAgent(ToolAgent):
    def __init__(
        self,
        system_prompt=TOOL_SYSTEM_PROMPT,
        parser_name="qwen",
        tool_map=list[MCPTool],  # MCP 工具列表
    ):
        ...
```

---

## 4. 其他 Agent 实现

### 4.1 CompetitionCodingAgent

用于编程竞赛任务的 Agent，通常与代码执行环境配合使用。

### 4.2 FrozenLakeAgent

用于 FrozenLake（冰湖）网格世界的 Agent，这是一个经典的强化学习环境。

### 4.3 SWEAgent

用于软件工程任务（SWE-bench）的 Agent，能够处理 GitHub 仓库中的问题。

### 4.4 MiniWobAgent

用于 MiniWoB 网页交互基准测试的 Agent。

### 4.5 WebArenaAgent

用于 WebArena 网页基准测试的 Agent。

---

## 5. 如何自定义 Agent

要创建自定义 Agent，需要继承 `BaseAgent` 并实现以下方法：

### 5.1 最小实现

```python
from rllm.agents import BaseAgent, Action, Step, Trajectory

class MyAgent(BaseAgent):
    def __init__(self):
        self._trajectory = Trajectory()
        self.messages = []
    
    def reset(self):
        self._trajectory = Trajectory()
        self.messages = []
    
    def update_from_env(self, observation, reward, done, info, **kwargs):
        # 处理环境反馈
        self.messages.append({"role": "user", "content": str(observation)})
        new_step = Step(observation=observation)
        self._trajectory.steps.append(new_step)
    
    def update_from_model(self, response, **kwargs) -> Action:
        # 处理模型响应
        self.messages.append({"role": "assistant", "content": response})
        cur_step = self.get_current_state()
        cur_step.chat_completions = self.chat_completions
        cur_step.model_response = response
        cur_step.action = Action(action=response)
        return Action(action=response)
    
    @property
    def chat_completions(self):
        return self.messages
    
    @property
    def trajectory(self):
        return self._trajectory
```

### 5.2 带工具使用的自定义 Agent

```python
class MyToolAgent(BaseAgent):
    def __init__(self, tools: list[Tool]):
        self._trajectory = Trajectory()
        self.messages = []
        self.tools = MultiTool(tool_map={t.name: t for t in tools})
        self.tool_parser = get_tool_parser("qwen")()
    
    def reset(self):
        self._trajectory = Trajectory()
        self.messages = [
            {"role": "system", "content": f"你可以使用以下工具：\n{self.tools.json}"}
        ]
    
    def update_from_env(self, observation, reward, done, info, **kwargs):
        # 处理工具输出
        if isinstance(observation, dict) and "tool_outputs" in observation:
            for tool_call_id, output in observation["tool_outputs"].items():
                self.messages.append({
                    "role": "tool",
                    "content": output,
                    "tool_call_id": tool_call_id,
                })
        elif isinstance(observation, str):
            self.messages.append({"role": "user", "content": observation})
    
    def update_from_model(self, response, **kwargs) -> Action:
        # 解析工具调用
        tool_calls = self.tool_parser.parse(response)
        self.messages.append({"role": "assistant", "content": response})
        
        new_step = Step(
            chat_completions=self.chat_completions,
            action=tool_calls,
            model_response=response,
        )
        self._trajectory.steps.append(new_step)
        
        return Action(action=tool_calls)
    
    @property
    def chat_completions(self):
        return self.messages
    
    @property
    def trajectory(self):
        return self._trajectory
```

---

## 6. Agent 在训练中的角色

在 rLLM 的训练流程中，Agent 扮演以下角色：

```
┌─────────────┐     ┌─────────────┐     ┌─────────────┐
│ Environment │────▶│    Agent    │────▶│   Engine    │
│             │     │             │     │             │
│  observation│     │ update_from │     │  generate   │
│             │◀────│   env/model │◀────│  response   │
│   reward    │     │             │     │             │
└─────────────┘     └──────┬──────┘     └─────────────┘
                           │
                           ▼
                    ┌─────────────┐
                    │ Trajectory  │
                    │ (for train) │
                    └─────────────┘
```

1. **接收观察**：`update_from_env()` 接收环境的观察和奖励
2. **生成动作**：`update_from_model()` 处理模型响应并返回动作
3. **收集轨迹**：通过 `trajectory` 属性获取完整的交互历史
4. **用于训练**：轨迹中的 token IDs、logprobs 等用于策略梯度更新

---

## 7. Agent 与 Workflow 的关系

Agent 可以独立使用，也可以嵌入到 Workflow 中：

**独立使用**：
```python
agent = MathAgent()
agent.reset()
agent.update_from_env(question, 0, False, {})
response = engine.generate(agent.chat_completions)
agent.update_from_model(response)
trajectory = agent.trajectory
```

**嵌入 Workflow**：
```python
class MyWorkflow(Workflow):
    def __init__(self, rollout_engine, ...):
        super().__init__(rollout_engine, ...)
        self.agent = MathAgent()  # Agent 作为 Workflow 的属性
    
    async def run(self, task, uid, **kwargs):
        self.reset(task, uid)  # 会调用 agent.reset()
        # ... 交互逻辑 ...
        self.commit(name="solver", agent=self.agent)  # 提交轨迹
        return self.collect_trajectories()
```

---

## 8. 总结

| 类 | 用途 | 特点 |
|----|------|------|
| `BaseAgent` | 抽象基类 | 定义标准接口，所有 Agent 必须继承 |
| `MathAgent` | 数学问题求解 | 支持 Chain of Thought，可控制思考内容保留 |
| `ToolAgent` | 工具使用 | 支持多工具调用，内置工具解析器 |
| `MCPToolAgent` | MCP 工具 | 支持 Model Context Protocol 工具 |
| `CompetitionCodingAgent` | 编程竞赛 | 与代码执行环境配合 |
| `FrozenLakeAgent` | 冰湖游戏 | 经典 RL 环境 |
| `SWEAgent` | 软件工程 | 处理 GitHub 仓库问题 |

理解 Agent 模块后，建议继续学习 Environment 模块，了解 Agent 如何与环境交互。
