# Tools 与 Rewards 模块详解

Tools（工具）和 Rewards（奖励）是 rLLM 中两个重要的模块。Tools 为 Agent 提供外部能力，Rewards 用于评估 Agent 的表现。

## Tools 模块结构

```
rllm/tools/
├── __init__.py                  # 导出所有工具类
├── tool_base.py                 # Tool 基类 + ToolCall + ToolOutput
├── registry.py                  # ToolRegistry 工具注册表
├── multi_tool.py                # MultiTool 多工具管理器
├── mcp_tool.py                  # MCP 工具集成
├── utils.py                     # 工具函数（function_to_dict）
├── code_tools/                  # 代码执行工具
│   └── python_interpreter.py    # PythonInterpreter
└── web_tools/                   # Web 搜索/提取工具
    ├── google_search.py         # GoogleSearchTool
    ├── tavily_search.py         # TavilySearchTool
    ├── tavily_extract.py        # TavilyExtractTool
    └── firecrawl.py             # FirecrawlTool
```

---

## 1. Tool 基类

### 1.1 类定义

```python
class Tool:
    """所有工具的抽象基类"""
    
    def __init__(
        self,
        name: str | None = None,           # 工具名称
        description: str | None = None,    # 工具描述
        function: Callable | None = None,  # 要转换的函数
    ):
        self.name = name
        self.description = description
        self.function = function
        
        if function is not None:
            # 自动从函数转换为工具格式
            self._json = function_to_dict(function)
            self.name = self._json["function"]["name"]
            self.description = self._json["function"]["description"]
        else:
            assert name is not None
            assert description is not None
            self._json = self.json
    
    @property
    def json(self) -> dict[str, Any]:
        """返回工具的标准 JSON 格式"""
        return self._json
    
    def forward(self, *args, **kwargs) -> ToolOutput:
        """同步工具实现"""
        if self.function is not None:
            try:
                output = self.function(*args, **kwargs)
                return ToolOutput(name=self.name, output=output)
            except Exception as e:
                return ToolOutput(name=self.name, error=f"{type(e).__name__} - {str(e)}")
        raise NotImplementedError
    
    async def async_forward(self, *args, **kwargs) -> ToolOutput:
        """异步工具实现"""
        return self.forward(*args, **kwargs)
    
    def __call__(self, *args, use_async=False, **kwargs):
        """使工具实例可调用"""
        if use_async is True:
            return self.async_forward(*args, **kwargs)
        elif use_async is False:
            return self.forward(*args, **kwargs)
        # 自动检测
        if has_async:
            return self.async_forward(*args, **kwargs)
        elif has_sync:
            return self.forward(*args, **kwargs)
```

### 1.2 工具调用和输出

**ToolCall**：

```python
@dataclass
class ToolCall:
    name: str                    # 工具名称
    arguments: dict[str, Any]    # 工具参数
    
    def to_dict(self):
        return {"name": self.name, "arguments": self.arguments}
```

**ToolOutput**：

```python
@dataclass
class ToolOutput:
    name: str                    # 工具名称
    output: str | list | dict | None = None  # 输出结果
    error: str | None = None     # 错误信息
    metadata: dict | None = None # 元数据
    
    def to_string(self) -> str:
        if self.error:
            return f"Error: {self.error}"
        elif self.output is None:
            return ""
        elif isinstance(self.output, list | dict):
            return json.dumps(self.output)
        else:
            return str(self.output)
```

### 1.3 工具 JSON 格式

工具的标准 JSON 格式与 OpenAI 的 function calling 格式一致：

```json
{
    "type": "function",
    "function": {
        "name": "calculator",
        "description": "执行数学计算",
        "parameters": {
            "type": "object",
            "properties": {
                "expression": {
                    "type": "string",
                    "description": "要计算的数学表达式"
                }
            },
            "required": ["expression"]
        }
    }
}
```

### 1.4 从函数创建工具

```python
def add(a: int, b: int) -> int:
    """计算两个数的和。
    
    Args:
        a: 第一个数
        b: 第二个数
    
    Returns:
        两数之和
    """
    return a + b

# 自动从函数创建工具
tool = Tool(function=add)
print(tool.json)
# {
#     "type": "function",
#     "function": {
#         "name": "add",
#         "description": "计算两个数的和",
#         "parameters": {
#             "type": "object",
#             "properties": {
#                 "a": {"type": "integer", "description": "第一个数"},
#                 "b": {"type": "integer", "description": "第二个数"}
#             },
#             "required": ["a", "b"]
#         }
#     }
# }

# 调用工具
result = tool(1, 2)
print(result.output)  # 3
```

---

## 2. MultiTool - 多工具管理器

`MultiTool` 用于管理多个工具：

```python
class MultiTool:
    def __init__(
        self,
        tools: list[str] | None = None,         # 工具名称列表
        tool_map: dict[str, type[Tool]] | None = None,  # 工具映射
    ):
        if tool_map is not None:
            self.tool_map = {name: tool_cls() for name, tool_cls in tool_map.items()}
        elif tools is not None:
            self.tool_map = {name: ToolRegistry.get_tool(name)() for name in tools}
        else:
            self.tool_map = {}
    
    @property
    def json(self) -> dict:
        """返回所有工具的 JSON 格式"""
        return {name: tool.json for name, tool in self.tool_map.items()}
    
    def __call__(self, tool_name: str, **kwargs) -> ToolOutput:
        """调用指定工具"""
        if tool_name not in self.tool_map:
            return ToolOutput(name=tool_name, error=f"Tool {tool_name} not found")
        return self.tool_map[tool_name](**kwargs)
```

---

## 3. ToolRegistry - 工具注册表

`ToolRegistry` 用于注册和查找工具：

```python
class ToolRegistry:
    _registry = {}
    
    @classmethod
    def register(cls, name: str, tool_cls: type[Tool]):
        cls._registry[name] = tool_cls
    
    @classmethod
    def get_tool(cls, name: str) -> type[Tool]:
        if name not in cls._registry:
            raise ValueError(f"Tool {name} not registered")
        return cls._registry[name]
```

---

## 4. 内置工具

### 4.1 PythonInterpreter

用于执行 Python 代码：

```python
class PythonInterpreter(Tool):
    def __init__(self, backend="e2b"):
        super().__init__(
            name="python_interpreter",
            description="执行 Python 代码",
        )
        self.backend = backend
    
    def forward(self, code: str) -> ToolOutput:
        try:
            # 执行代码
            result = self._execute_code(code)
            return ToolOutput(name=self.name, output=result)
        except Exception as e:
            return ToolOutput(name=self.name, error=str(e))
```

### 4.2 GoogleSearchTool

用于 Google 搜索：

```python
class GoogleSearchTool(Tool):
    def __init__(self):
        super().__init__(
            name="google_search",
            description="使用 Google 搜索信息",
        )
    
    def forward(self, query: str, num_results: int = 5) -> ToolOutput:
        # 执行搜索
        results = self._search(query, num_results)
        return ToolOutput(name=self.name, output=results)
```

### 4.3 TavilySearchTool

用于 Tavily 搜索：

```python
class TavilySearchTool(Tool):
    def __init__(self):
        super().__init__(
            name="tavily_search",
            description="使用 Tavily 搜索信息",
        )
    
    def forward(self, query: str) -> ToolOutput:
        results = self._search(query)
        return ToolOutput(name=self.name, output=results)
```

---

## Rewards 模块结构

```
rllm/rewards/
├── __init__.py                  # 导出奖励相关类
├── reward_fn.py                 # RewardFunction 协议 + 奖励函数
├── reward_types.py              # RewardConfig, RewardType, RewardInput, RewardOutput
├── math_reward.py               # 数学奖励
├── code_reward.py               # 代码奖励
├── search_reward.py             # 搜索奖励
├── countdown_reward.py          # 倒计时奖励
├── math_utils/                  # 数学评估工具
└── code_utils/                  # 代码评估工具
```

---

## 5. RewardFunction 协议

`RewardFunction` 是一个 Protocol，定义了奖励函数的标准接口：

```python
@runtime_checkable
class RewardFunction(Protocol):
    """奖励函数协议"""
    
    def __call__(self, task_info: dict, action: str) -> RewardOutput:
        """
        计算 Agent 动作的奖励
        
        Args:
            task_info: 任务字典，包含问题、答案等
            action: Agent 的响应
        
        Returns:
            RewardOutput: 计算出的奖励值
        """
        ...
```

---

## 6. RewardOutput

```python
@dataclass
class RewardOutput:
    reward: float                  # 奖励值
    metadata: dict = field(default_factory=dict)  # 元数据
    is_correct: bool = False       # 是否正确
```

---

## 7. 内置奖励函数

### 7.1 zero_reward

简单的零奖励函数，用作占位符：

```python
def zero_reward(task_info: dict, action: str) -> RewardOutput:
    """始终返回 0.0 的奖励函数"""
    return RewardOutput(reward=0.0, metadata={})
```

### 7.2 math_reward_fn

数学任务奖励函数：

```python
def math_reward_fn(task_info: dict, action: str) -> RewardOutput:
    """
    数学任务奖励函数
    
    评估逻辑：
    1. 从 action 中提取答案
    2. 与 ground_truth 比较
    3. 使用符号计算验证等价性
    """
    reward_config = RewardConfig()
    reward_fn = RewardMathFn(reward_config)
    if isinstance(action, Action):
        action = action.action
    return reward_fn(task_info, action)
```

### 7.3 code_reward_fn

代码任务奖励函数：

```python
def code_reward_fn(task_info: dict, action: str) -> RewardOutput:
    """
    代码任务奖励函数
    
    评估逻辑：
    1. 从 action 中提取代码
    2. 在沙箱中执行测试用例
    3. 根据通过的测试用例数量计算奖励
    """
    reward_config = RewardConfig()
    reward_fn = RewardCodeFn(reward_config)
    if isinstance(action, Action):
        action = action.action
    return reward_fn(task_info, action)
```

### 7.4 search_reward_fn

搜索任务奖励函数：

```python
def search_reward_fn(task_info: dict, action: str) -> RewardOutput:
    """
    搜索任务奖励函数
    
    评估逻辑：
    1. 检查答案是否包含关键信息
    2. 使用 F1 分数评估相似度
    """
    reward_config = RewardConfig()
    reward_fn = RewardSearchFn(reward_config)
    if isinstance(action, Action):
        action = action.action
    reward_input = RewardInput(task_info=task_info, action=action)
    return reward_fn(reward_input)
```

---

## 8. 自定义奖励函数

### 8.1 简单奖励函数

```python
def my_reward_fn(task_info: dict, action: str) -> RewardOutput:
    """自定义奖励函数"""
    ground_truth = task_info.get("ground_truth", "")
    
    # 简单的字符串匹配
    if action.strip().lower() == ground_truth.strip().lower():
        return RewardOutput(reward=1.0, is_correct=True)
    else:
        return RewardOutput(reward=0.0, is_correct=False)
```

### 8.2 带元数据的奖励函数

```python
def detailed_reward_fn(task_info: dict, action: str) -> RewardOutput:
    """带详细元数据的奖励函数"""
    ground_truth = task_info.get("ground_truth", "")
    
    # 计算多种指标
    exact_match = action.strip() == ground_truth.strip()
    contains_keyword = "keyword" in action.lower()
    
    # 组合奖励
    reward = 0.0
    if exact_match:
        reward += 1.0
    if contains_keyword:
        reward += 0.5
    
    return RewardOutput(
        reward=reward,
        is_correct=exact_match,
        metadata={
            "exact_match": exact_match,
            "contains_keyword": contains_keyword,
        }
    )
```

### 8.3 使用 RewardMathFn

```python
from rllm.rewards import RewardMathFn, RewardConfig

class CustomMathReward:
    def __init__(self):
        self.reward_config = RewardConfig()
        self.reward_fn = RewardMathFn(self.reward_config)
    
    def __call__(self, task_info: dict, action: str) -> RewardOutput:
        return self.reward_fn(task_info, action)
```

---

## 9. 奖励函数在训练中的使用

```
┌─────────────┐
│ Environment │
│             │
│ step()      │──── 动作 ────▶ reward_fn(task_info, action)
│             │◀─── 奖励 ─────│
└─────────────┘               │
                              ▼
                       ┌─────────────┐
                       │ RewardOutput│
                       │             │
                       │ reward: 1.0 │
                       │ is_correct: │
                       │   True      │
                       └─────────────┘
```

---

## 10. 总结

| 模块 | 类/函数 | 用途 |
|------|--------|------|
| **Tools** | `Tool` | 工具基类 |
| | `ToolCall` | 工具调用表示 |
| | `ToolOutput` | 工具输出表示 |
| | `MultiTool` | 多工具管理器 |
| | `ToolRegistry` | 工具注册表 |
| | `PythonInterpreter` | Python 代码执行 |
| | `GoogleSearchTool` | Google 搜索 |
| **Rewards** | `RewardFunction` | 奖励函数协议 |
| | `RewardOutput` | 奖励输出 |
| | `zero_reward` | 零奖励 |
| | `math_reward_fn` | 数学奖励 |
| | `code_reward_fn` | 代码奖励 |
| | `search_reward_fn` | 搜索奖励 |
