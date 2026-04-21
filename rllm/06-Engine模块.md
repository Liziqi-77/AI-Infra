# Engine 模块详解

Engine（引擎）是 rLLM 中负责模型推理和执行调度的核心组件。本文档详细介绍 Engine 模块的设计、核心类和具体实现。

## 模块结构

```
rllm/engine/
├── __init__.py                      # 懒加载导入所有引擎
├── agent_execution_engine.py        # AgentExecutionEngine Agent 执行引擎
├── agent_workflow_engine.py         # AgentWorkflowEngine 工作流执行引擎
├── agent_sdk_engine.py              # AgentSdkEngine SDK 执行引擎
└── rollout/                         # 推理引擎
    ├── rollout_engine.py            # RolloutEngine 基类 + ModelOutput
    ├── openai_engine.py             # OpenAIEngine OpenAI 兼容 API
    ├── verl_engine.py               # VerlEngine 分布式推理
    ├── tinker_engine.py             # TinkerEngine 单机推理
    └── fireworks_engine.py          # FireworksEngine
```

---

## 1. RolloutEngine - 推理引擎基类

`RolloutEngine` 是所有推理引擎的基类，定义了模型推理的标准接口。

### 1.1 类定义

```python
class RolloutEngine:
    def __init__(self, *args, **kwargs):
        pass
    
    async def get_model_response(self, messages: list[dict], **kwargs) -> ModelOutput:
        """获取模型响应"""
        raise NotImplementedError
    
    async def wake_up(self):
        """唤醒引擎（如加载模型到 GPU）"""
        pass
    
    async def sleep(self):
        """休眠引擎（如释放 GPU 内存）"""
        pass
```

### 1.2 ModelOutput 数据类

`ModelOutput` 是推理引擎返回的标准化输出：

```python
@dataclass
class ModelOutput:
    text: str | None = None                    # 纯文本输出
    content: str | None = None                 # 主要内容（不含思考）
    reasoning: str | None = None               # 推理/思考内容
    tool_calls: list[ToolCall] | None = None   # 工具调用列表
    prompt_ids: list[int] | None = None        # prompt token IDs
    completion_ids: list[int] | None = None    # 生成 token IDs
    multi_modal_inputs: dict | None = None     # 多模态输入
    logprobs: list[float] | None = None        # 生成的对数概率
    prompt_logprobs: list[float] | None = None # prompt 的对数概率
    prompt_length: int = 0                     # prompt 长度
    completion_length: int = 0                 # 生成长度
    finish_reason: str | None = None           # 完成原因
    
    def to_dict(self) -> dict: ...
    @classmethod
    def from_dict(cls, data: dict) -> "ModelOutput": ...
```

**关键字段说明**：

| 字段 | 说明 |
|------|------|
| `text` | 模型的完整文本输出 |
| `content` | 主要内容部分（可能去除了思考内容） |
| `reasoning` | 模型的推理/思考过程（如 `<think>...</think>` 内的内容） |
| `prompt_ids` | 输入文本的 token IDs，用于策略梯度计算 |
| `completion_ids` | 生成文本的 token IDs，与 logprobs 一一对应 |
| `logprobs` | 每个生成 token 的对数概率，RL 训练的关键数据 |
| `finish_reason` | 完成原因，如 "stop"（正常结束）、"length"（超长） |

---

## 2. OpenAIEngine - OpenAI 兼容推理引擎

`OpenAIEngine` 通过 OpenAI 兼容的 API 进行模型推理。

### 2.1 类定义

```python
class OpenAIEngine(RolloutEngine):
    def __init__(
        self,
        model: str = "",                    # 模型名称
        tokenizer=None,                     # Tokenizer
        chat_parser=None,                   # 聊天模板解析器
        max_prompt_length: int = 4096,      # 最大 prompt 长度
        max_response_length: int = 4096,    # 最大响应长度
        max_model_length: int | None = None, # 最大模型上下文长度
        api_retries: int = 3,               # API 重试次数
        base_url: str = "https://api.openai.com/v1",
        api_key: str = os.getenv("OPENAI_API_KEY"),
        sampling_params: dict | None = None, # 采样参数
        tools: list[Tool | dict] = None,    # 工具列表
        accumulate_reasoning: bool = False, # 是否累积推理内容
        **kwargs,
    ):
        self.model = model
        self.max_prompt_length = max_prompt_length
        self.max_response_length = max_response_length
        self.max_model_length = max_model_length or (max_prompt_length + max_response_length - 1)
        self.api_retries = api_retries
        self.sampling_params = sampling_params or {}
        self.tools = tools or []
        self.accumulate_reasoning = accumulate_reasoning
        
        # 如果有 tokenizer，使用 completion 端点；否则使用 chat completions 端点
        if tokenizer is not None:
            self.chat_parser = chat_parser or ChatTemplateParser.get_parser(tokenizer)
            self._use_chat_completions = False
        else:
            self._use_chat_completions = True
        
        self.client = openai.AsyncOpenAI(base_url=base_url, api_key=api_key)
```

### 2.2 两种推理模式

**Chat Completions 模式**：

当没有提供 tokenizer 时，使用 OpenAI 的 chat completions 端点：

```python
async def chat_completion(self, messages: list[dict], **kwargs) -> ModelOutput:
    response = await self.client.chat.completions.create(
        model=self.model,
        messages=messages,
        max_tokens=self.max_response_length,
        **self.sampling_params,
    )
    
    # 解析响应
    choice = response.choices[0]
    content = choice.message.content or ""
    
    # 提取思考内容
    reasoning = self._extract_reasoning(content)
    
    return ModelOutput(
        text=content,
        content=content,
        reasoning=reasoning,
        finish_reason=choice.finish_reason,
    )
```

**Completion 模式**：

当提供了 tokenizer 时，使用原始 completion 端点，可以获取 token IDs 和 logprobs：

```python
async def completion(self, messages: list[dict], **kwargs) -> ModelOutput:
    # 使用 chat_parser 将消息转换为 token IDs
    prompt_ids, prompt_length = self.chat_parser.tokenize_and_mask(messages)
    
    # 调用 completion 端点
    response = await self.client.completions.create(
        model=self.model,
        prompt=prompt_ids,
        max_tokens=self.max_response_length,
        logprobs=0,  # 请求所有 token 的 logprobs
        **self.sampling_params,
    )
    
    # 解析响应
    return ModelOutput(
        text=response.text,
        prompt_ids=prompt_ids,
        completion_ids=response.token_ids,
        logprobs=response.logprobs,
        finish_reason=response.finish_reason,
    )
```

### 2.3 多模态支持

OpenAIEngine 支持多模态输入（如图像）：

```python
def _convert_messages_to_openai_format(self, messages: list[dict]) -> list[dict]:
    converted_messages = []
    for message in messages:
        if "images" in message and message["images"]:
            content = [{"type": "text", "text": message["content"]}]
            for img in message["images"]:
                base64_image = self._pil_to_base64(img)
                content.append({
                    "type": "image_url",
                    "image_url": {"url": f"data:image/png;base64,{base64_image}"},
                })
            converted_messages.append({"role": message["role"], "content": content})
        else:
            converted_messages.append(message)
    return converted_messages
```

---

## 3. VerlEngine - 分布式推理引擎

`VerlEngine` 使用 verl 框架进行分布式 GPU 推理。

### 3.1 特点

- 支持多 GPU 并行推理
- 基于 vLLM 或 SGLang 后端
- 与 verl 训练框架无缝集成
- 支持模型并行和张量并行

### 3.2 使用场景

- 大规模分布式训练
- 需要高吞吐量的推理
- 生产环境部署

---

## 4. TinkerEngine - 单机推理引擎

`TinkerEngine` 用于单机/CPU 推理。

### 4.1 特点

- 设置简单
- 适合快速原型开发
- 支持 CPU 和单 GPU
- 与 tinker 训练框架集成

### 4.2 使用场景

- 小规模实验
- 快速原型开发
- 资源受限环境

---

## 5. AgentExecutionEngine - Agent 执行引擎

`AgentExecutionEngine` 负责运行多个并行的 Agent-Environment 对，收集轨迹。

### 5.1 核心功能

- 并行运行 N 个 Agent-Environment 对
- 支持重试逻辑
- 支持多种模式：Text/Token/Conversation/Step
- 收集完整的训练轨迹

### 5.2 工作流程

```
┌─────────────────────────────────────────────────┐
│            AgentExecutionEngine                  │
│                                                   │
│  ┌─────────┐  ┌─────────┐       ┌─────────┐     │
│  │ Agent 0 │  │ Agent 1 │  ...  │ Agent N │     │
│  │   +     │  │   +     │       │   +     │     │
│  │ Env 0   │  │ Env 1   │       │ Env N   │     │
│  └────┬────┘  └────┬────┘       └────┬────┘     │
│       │            │                  │           │
│       └────────────┼──────────────────┘           │
│                    │                              │
│                    ▼                              │
│          ┌─────────────────┐                      │
│          │  Trajectories   │                      │
│          │  Collected      │                      │
│          └─────────────────┘                      │
└─────────────────────────────────────────────────┘
```

---

## 6. AgentWorkflowEngine - 工作流执行引擎

`AgentWorkflowEngine` 基于工作流池执行推理。

### 6.1 核心功能

- 管理并行工作流池
- 支持重试逻辑
- 与 verl 兼容的输出转换
- 支持多种工作流类型

### 6.2 使用场景

- 复杂的多 Agent 场景
- 需要自定义工作流逻辑
- 现代训练范式

---

## 7. 引擎选择指南

| 引擎 | 适用场景 | 特点 |
|------|---------|------|
| `OpenAIEngine` | API 调用、快速测试 | 支持任何 OpenAI 兼容 API |
| `VerlEngine` | 分布式训练 | 多 GPU、高吞吐量 |
| `TinkerEngine` | 单机训练 | 简单设置、适合原型 |
| `FireworksEngine` | Fireworks API | 特定于 Fireworks 服务 |

---

## 8. 引擎在训练中的角色

```
┌─────────────────────────────────────────────────┐
│                  Training Loop                    │
│                                                    │
│  ┌─────────────┐                                 │
│  │  Workflow   │                                 │
│  │  /Agent     │──── 需要生成响应 ──────────────┐ │
│  │             │                               │ │
│  └─────────────┘                               ▼ │
│                                         ┌─────────────┐
│                                         │ RolloutEngine│
│                                         │              │
│                                         │ get_model_   │
│                                         │ response()   │
│                                         │              │
│                                         │ 返回 ModelOutput│
│                                         └──────┬──────┘
│                                                │
│                                                ▼
│                                         ┌─────────────┐
│                                         │  Step 创建   │
│                                         │  (含 token   │
│                                         │   IDs,       │
│                                         │   logprobs)  │
│                                         └─────────────┘
└─────────────────────────────────────────────────┘
```

---

## 9. 总结

Engine 模块是 rLLM 中连接模型和训练基础设施的桥梁。理解 Engine 模块后，建议继续学习 Trainer 模块，了解如何将推理结果用于训练。
