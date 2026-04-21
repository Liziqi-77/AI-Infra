# Data 与 Parser 模块详解

Data（数据）和 Parser（解析器）是 rLLM 中两个重要的基础设施模块。

## Data 模块

### Dataset 类

轻量级数据集类，兼容 `torch.utils.data.DataLoader`。

**核心方法：**
- `repeat(n)`：重复数据集 n 次
- `shuffle(seed)`：打乱数据集
- `select(indices)`：选择子集
- `load_data(path)`：从文件加载（支持 json/jsonl/csv/parquet/arrow）

### DatasetRegistry

数据集注册表，存储在 `~/.rllm/datasets/`。

```python
from rllm.data import DatasetRegistry

# 注册
DatasetRegistry.register_dataset(name="gsm8k", data=hf_data, split="train")

# 加载
dataset = DatasetRegistry.load_dataset("gsm8k", split="train")
```

### 数据集类型

预置的训练/测试数据集枚举：
- **Math**: AIME, AMC, MATH, GSM8K, OMNI_MATH 等
- **Code**: TACO, APPS, CODEFORCES, LIVECODEBENCH 等
- **Web**: GAIA

### Transforms

将 HuggingFace 格式转换为 rLLM 标准格式（question, ground_truth, data_source）。

---

## Parser 模块

### ChatTemplateParser

将消息列表转换为模型 prompt。

```python
parser = ChatTemplateParser.get_parser(tokenizer)  # 自动选择
prompt = parser.parse(messages, add_generation_prompt=True)
```

支持的模型：DeepSeek, Qwen, Llama, GPT-OSS, Kimi-K2

### ToolParser

解析模型输出中的工具调用。
- **R1ToolParser**: R1 格式
- **QwenToolParser**: Qwen 格式
