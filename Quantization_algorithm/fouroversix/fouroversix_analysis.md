# FourOverSix 算法详细分析文档

## 目录
1. [FourOverSix算法框架详解](#1-fouroversix算法框架详解)
2. [量化过程分析](#2-量化过程分析)
3. [反量化过程分析](#3-反量化过程分析)
4. [核心实现详解](#4-核心实现详解)

---

## 1. FourOverSix算法框架详解

### 1.1 项目概述

FourOverSix (4/6) 是一个针对NVFP4格式的量化算法，其核心创新是**自适应块缩放(Adaptive Block Scaling)**。该项目提供了完整的量化、反量化和矩阵乘法实现，支持模型推理和训练。

**项目地址**: https://github.com/mit-han-lab/fouroversix  
**论文**: [Four Over Six: More Accurate NVFP4 Quantization with Adaptive Block Scaling](https://arxiv.org/abs/2512.02010)

### 1.2 代码仓库结构

```
fouroversix/
├── src/fouroversix/           # 核心源代码
│   ├── quantize/              # 量化模块
│   │   ├── frontend.py        # 前端API接口
│   │   ├── backend.py         # 后端抽象基类
│   │   ├── config.py          # 量化配置
│   │   ├── quantized_tensor.py # 量化张量类
│   │   ├── pytorch/           # PyTorch后端实现
│   │   ├── triton/            # Triton后端实现
│   │   └── cuda/              # CUDA后端实现
│   ├── matmul/                # 矩阵乘法模块
│   │   ├── frontend.py        # 矩阵乘法API
│   │   ├── cutlass/           # CUTLASS实现
│   │   └── pytorch/           # PyTorch实现
│   ├── model/                 # 模型量化模块
│   │   ├── config.py          # 模型量化配置
│   │   ├── quantize.py        # 模型量化工具
│   │   └── modules/           # 量化模块实现
│   └── utils.py               # 工具函数和枚举定义
├── scripts/ptq/               # PTQ实验脚本
│   ├── __main__.py            # 入口脚本
│   ├── experiment.py          # 实验管理
│   ├── evaluators/            # 各种PTQ方法评估器
│   └── coordinators/          # 本地/云端协调器
└── tests/                     # 测试代码
    └── test_correctness.py    # 正确性测试
```

### 1.3 测试框架详解

#### 1.3.1 PTQ测试入口

测试FourOverSix算法的主要入口是 `scripts/ptq/__main__.py`，通过命令行参数控制测试：

```bash
# 使用4/6量化测试
python -m scripts.ptq --model-name meta-llama/Llama-3.2-1B --ptq-method rtn --task wikitext

# 标准NVFP4量化测试
python -m scripts.ptq --model-name meta-llama/Llama-3.2-1B --ptq-method rtn --task wikitext --a-scale-rule static_6 --w-scale-rule static_6
```

#### 1.3.2 测试框架核心组件

**1. 协调器 (Coordinators)**
- `LocalEvaluationCoordinator`: 本地测试协调器
- `ModalEvaluationCoordinator`: 云端测试协调器（使用Modal平台）

**2. 评估器 (Evaluators)**
- `RTNEvaluator`: Round-to-Nearest量化评估器
- `AWQEvaluator`: AWQ量化评估器
- `GPTQEvaluator`: GPTQ量化评估器
- `SmoothQuantEvaluator`: SmoothQuant评估器
- `SpinQuantEvaluator`: SpinQuant评估器
- `HighPrecisionEvaluator`: 高精度基线评估器

**3. 评估框架**
- `lm_eval`: 使用lm-evaluation-harness进行评估
- `inspect_ai`: 使用Inspect AI进行评估

#### 1.3.3 测试流程

```
1. 加载模型配置
   ↓
2. 创建量化配置 (ModelQuantizationConfig)
   ↓
3. 应用量化 (quantize_model)
   ↓
4. 运行评估任务 (lm_eval/inspect_ai)
   ↓
5. 收集结果并存储
```

### 1.4 核心模块交互图

```
┌─────────────────────────────────────────────────────────────┐
│                     用户API层                                │
│  quantize_to_fp4() / quantize_model() / fp4_matmul()        │
└─────────────────────────────────────────────────────────────┘
                              ↓
┌─────────────────────────────────────────────────────────────┐
│                     前端层 (Frontend)                        │
│  QuantizationConfig / ModelQuantizationConfig               │
└─────────────────────────────────────────────────────────────┘
                              ↓
┌─────────────────────────────────────────────────────────────┐
│                     后端抽象层 (Backend)                     │
│  QuantizeBackendBase / MatmulBackendBase                    │
└─────────────────────────────────────────────────────────────┘
                              ↓
┌───────────────┬───────────────┬───────────────┐
│ PyTorch后端   │ Triton后端    │ CUDA后端      │
│ (reference)   │ (训练支持)    │ (推理优化)    │
└───────────────┴───────────────┴───────────────┘
```

### 1.5 关键配置类

#### 1.5.1 QuantizationConfig (张量级配置)

```python
@dataclass
class QuantizationConfig:
    backend: QuantizeBackend | None = None      # 后端选择
    block_scale_2d: bool = False                # 2D块缩放
    dtype: DataType = DataType.nvfp4            # 数据类型 (nvfp4/mxfp4)
    rbits: int = -1                             # 随机舍入位数
    rht: bool = False                           # 随机Hadamard变换
    round_style: RoundStyle = RoundStyle.nearest # 舍入方式
    scale_rule: ScaleRule = ScaleRule.mse       # 缩放规则
    transpose: bool = False                     # 是否转置
```

#### 1.5.2 ModelQuantizationConfig (模型级配置)

```python
@dataclass
class ModelQuantizationConfig:
    activation_scale_rule: ScaleRule | None = None  # 激活值缩放规则
    weight_scale_rule: ScaleRule | None = None      # 权重缩放规则
    dtype: DataType = DataType.nvfp4                # 数据类型
    quantize_backend: QuantizeBackend | None = None # 量化后端
    matmul_backend: MatmulBackend | None = None     # 矩阵乘法后端
    weight_scale_2d: bool = False                   # 权重2D块缩放
    modules_to_not_convert: list[str] = ["lm_head"] # 不转换的模块
```

### 1.6 数据类型和格式

#### 1.6.1 NVFP4 vs MXFP4

| 特性 | NVFP4 | MXFP4 |
|------|-------|-------|
| 块大小 | 16 | 32 |
| 缩放因子类型 | E4M3 (float8) | E8M0 (uint8) |
| 最大E2M1值 | 6 (标准) / 4 (4/6) | 6 |
| 支持自适应缩放 | 是 | 否 |

#### 1.6.2 缩放规则 (ScaleRule)

```python
class ScaleRule(str, Enum):
    static_6 = "static_6"    # 固定最大值为6（标准NVFP4）
    static_4 = "static_4"    # 固定最大值为4
    mse = "mse"              # MSE误差最小化选择（4/6核心）
    mae = "mae"              # MAE误差最小化选择
    abs_max = "abs_max"      # 最大绝对误差最小化选择
```

---

## 1.7 代码执行流分析（以标准NVFP4量化测试为例）

本节详细分析执行标准NVFP4量化测试命令后的完整代码执行流程。

### 1.7.1 测试命令

```bash
python -m scripts.ptq \
    --model-name meta-llama/Llama-3.2-1B \
    --ptq-method rtn \
    --task wikitext \
    --a-scale-rule static_6 \
    --w-scale-rule static_6
```

### 1.7.2 执行流程概览

```
┌─────────────────────────────────────────────────────────────────────────┐
│ 1. 命令行入口                                                            │
│    scripts/ptq/__main__.py::cli()                                        │
└─────────────────────────────────────────────────────────────────────────┘
                                    ↓
┌─────────────────────────────────────────────────────────────────────────┐
│ 2. 创建本地协调器                                                        │
│    LocalEvaluationCoordinator                                            │
└─────────────────────────────────────────────────────────────────────────┘
                                    ↓
┌─────────────────────────────────────────────────────────────────────────┐
│ 3. 启动多进程Worker                                                      │
│    LocalEvaluationCoordinator.start() → worker()                         │
└─────────────────────────────────────────────────────────────────────────┘
                                    ↓
┌─────────────────────────────────────────────────────────────────────────┐
│ 4. 获取评估器                                                            │
│    get_evaluator(PTQMethod.rtn) → RTNEvaluator                           │
└─────────────────────────────────────────────────────────────────────────┘
                                    ↓
┌─────────────────────────────────────────────────────────────────────────┐
│ 5. 执行评估                                                              │
│    RTNEvaluator.evaluate()                                               │
└─────────────────────────────────────────────────────────────────────────┘
                                    ↓
┌─────────────────────────────────────────────────────────────────────────┐
│ 6. 量化模型                                                              │
│    RTNEvaluator.quantize_model() → HFFourOverSixConfig                   │
└─────────────────────────────────────────────────────────────────────────┘
                                    ↓
┌─────────────────────────────────────────────────────────────────────────┐
│ 7. 模型加载与量化                                                        │
│    AutoModelForCausalLM.from_pretrained() → 自动调用量化模块              │
└─────────────────────────────────────────────────────────────────────────┘
                                    ↓
┌─────────────────────────────────────────────────────────────────────────┐
│ 8. 线性层替换                                                            │
│    nn.Linear → FourOverSixLinear                                         │
└─────────────────────────────────────────────────────────────────────────┘
                                    ↓
┌─────────────────────────────────────────────────────────────────────────┐
│ 9. 权重量化                                                              │
│    quantize_to_fp4() → PyTorchQuantizeBackend.quantize_to_fp4()          │
└─────────────────────────────────────────────────────────────────────────┘
                                    ↓
┌─────────────────────────────────────────────────────────────────────────┐
│ 10. 运行评估任务                                                         │
│     lm_eval evaluator.simple_evaluate()                                  │
└─────────────────────────────────────────────────────────────────────────┘
                                    ↓
┌─────────────────────────────────────────────────────────────────────────┐
│ 11. 模型推理                                                             │
│     FourOverSixLinear.forward() → fp4_matmul()                           │
└─────────────────────────────────────────────────────────────────────────┘
                                    ↓
┌─────────────────────────────────────────────────────────────────────────┐
│ 12. 保存结果                                                             │
│     LocalEvaluationCoordinator.save_results()                            │
└─────────────────────────────────────────────────────────────────────────┘
```

### 1.7.3 函数调用流程图（详细版）

本节展示完整的函数调用链，帮助走读代码。

```
═══════════════════════════════════════════════════════════════════════════════
【阶段1: 命令行解析与协调器初始化】
═══════════════════════════════════════════════════════════════════════════════

python -m scripts.ptq --model-name meta-llama/Llama-3.2-1B --ptq-method rtn ...
    │
    ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│ scripts/ptq/__main__.py                                                      │
│ ┌─────────────────────────────────────────────────────────────────────────┐ │
│ │ cli()  [Click命令行入口函数]                                              │ │
│ │   ├── 解析命令行参数                                                      │ │
│ │   │   ├── model_names = ["meta-llama/Llama-3.2-1B"]                     │ │
│ │   │   ├── ptq_methods = [PTQMethod.rtn]                                 │ │
│ │   │   ├── tasks = ["wikitext"]                                          │ │
│ │   │   ├── activation_scale_rule = ScaleRule.static_6                    │ │
│ │   │   └── weight_scale_rule = ScaleRule.static_6                        │ │
│ │   │                                                                      │ │
│ │   ├── use_modal = False (本地测试)                                       │ │
│ │   │                                                                      │ │
│ │   └──▶ coordinator = LocalEvaluationCoordinator(group_name)             │ │
│ │          │                                                               │ │
│ │          │  scripts/ptq/coordinators/local.py                            │ │
│ │          │  ┌────────────────────────────────────────────────────────┐   │ │
│ │          └──│ __init__(self, group_name)                             │   │ │
│ │             │   ├── self.database_path = "results.db"               │   │ │
│ │             │   └── self.group_name = group_name                    │   │ │
│ │             └────────────────────────────────────────────────────────┘   │ │
│ │                                                                          │ │
│ │   └──▶ coordinator.start(model_names, ptq_methods, tasks, **kwargs)     │ │
│ └─────────────────────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
═══════════════════════════════════════════════════════════════════════════════
【阶段2: 多进程Worker启动】
═══════════════════════════════════════════════════════════════════════════════

┌─────────────────────────────────────────────────────────────────────────────┐
│ scripts/ptq/coordinators/local.py                                            │
│ ┌─────────────────────────────────────────────────────────────────────────┐ │
│ │ start(self, model_names, ptq_methods, tasks, device, **kwargs)          │ │
│ │   │                                                                      │ │
│ │   ├── multiprocessing.set_start_method("spawn", force=True)             │ │
│ │   │                                                                      │ │
│ │   ├── manager = multiprocessing.Manager()                               │ │
│ │   ├── task_queue = manager.Queue()      ← 任务队列                       │ │
│ │   ├── result_queue = manager.Queue()    ← 结果队列                       │ │
│ │   │                                                                      │ │
│ │   ├── num_workers = torch.cuda.device_count()  ← GPU数量                │ │
│ │   │                                                                      │ │
│ │   ├── 【启动Worker进程】                                                  │ │
│ │   │   for gpu_id in range(num_workers):                                 │ │
│ │   │       p = multiprocessing.Process(target=self.worker, args=...)     │ │
│ │   │       p.start()                                                      │ │
│ │   │       workers.append(p)                                              │ │
│ │   │                                                                      │ │
│ │   ├── 【运行校准任务】                                                    │ │
│ │   │   └──▶ self.run_calibration_tasks(...)                              │ │
│ │   │          └──▶ evaluator_cls.get_calibration_tasks(...)              │ │
│ │   │                 └── 返回 [] (RTN不需要校准)                          │ │
│ │   │                                                                      │ │
│ │   ├── 【分发评估任务】                                                    │ │
│ │   │   for model_name, ptq_method in product(model_names, ptq_methods):  │ │
│ │   │       tasks_to_evaluate = self.get_tasks_to_evaluate(...)           │ │
│ │   │       │                                                              │ │
│ │   │       │  scripts/ptq/coordinators/base.py                            │ │
│ │   │       │  ┌────────────────────────────────────────────────────────┐  │ │
│ │   │       └──│ get_tasks_to_evaluate(self, model_name, ptq_method)   │  │ │
│ │   │          │   └── 检查数据库，返回未评估的任务                      │  │ │
│ │   │          └────────────────────────────────────────────────────────┘  │ │
│ │   │                                                                      │ │
│ │   │       task_queue.put((model_name, ptq_method, kwargs))              │ │
│ │   │                                                                      │ │
│ │   ├── 【发送关闭信号】                                                    │ │
│ │   │   for _ in range(num_workers):                                      │ │
│ │   │       task_queue.put(None)                                          │ │
│ │   │                                                                      │ │
│ │   ├── 【收集结果】                                                        │ │
│ │   │   for _ in range(experiments):                                      │ │
│ │   │       result = result_queue.get()                                   │ │
│ │   │       └──▶ self.save_results(*result)                               │ │
│ │   │                                                                      │ │
│ │   └── for p in workers: p.join()  ← 等待所有Worker结束                  │ │
│ └─────────────────────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
═══════════════════════════════════════════════════════════════════════════════
【阶段3: Worker进程执行评估】
═══════════════════════════════════════════════════════════════════════════════

┌─────────────────────────────────────────────────────────────────────────────┐
│ scripts/ptq/coordinators/local.py                                            │
│ ┌─────────────────────────────────────────────────────────────────────────┐ │
│ │ worker(self, device, task_queue, result_queue)                          │ │
│ │   │                                                                      │ │
│ │   └── while True:                                                        │ │
│ │         worker_task = task_queue.get()                                  │ │
│ │         if worker_task is None: break  ← 收到关闭信号                   │ │
│ │         │                                                                │ │
│ │         model_name, ptq_method, kwargs = worker_task                    │ │
│ │         │                                                                │ │
│ │         └──▶ results = self.evaluate(model_name, ptq_method, **kwargs)  │ │
│ │                │                                                         │ │
│ │                │  scripts/ptq/coordinators/local.py                      │ │
│ │                │  ┌────────────────────────────────────────────────────┐ │ │
│ │                └──│ evaluate(self, model_name, ptq_method, **kwargs)  │ │ │
│ │                   │   │                                                │ │ │
│ │                   │   └──▶ evaluator_cls = get_evaluator(ptq_method)  │ │ │
│ │                   │          │                                         │ │ │
│ │                   │          │  scripts/ptq/evaluators/__init__.py     │ │ │
│ │                   │          │  ┌────────────────────────────────────┐│ │ │
│ │                   │          └──│ get_evaluator(ptq_method)          ││ │ │
│ │                   │             │   if ptq_method == PTQMethod.rtn:  ││ │ │
│ │                   │             │       return RTNEvaluator          ││ │ │
│ │                   │             └────────────────────────────────────┘│ │ │
│ │                   │                                                      │ │
│ │                   └──▶ return evaluator_cls().evaluate(...)            │ │
│ │                          │                                               │ │
│ │                          ▼                                               │ │
│ │                   ┌──────────────────────────────────────────────────┐  │ │
│ │                   │ RTNEvaluator().evaluate(...)                     │  │ │
│ │                   └──────────────────────────────────────────────────┘  │ │
│ │                                                                          │ │
│ │         result_queue.put((model_name, ptq_method, kwargs, results))     │ │
│ └─────────────────────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
═══════════════════════════════════════════════════════════════════════════════
【阶段4: 评估器执行量化与评估】
═══════════════════════════════════════════════════════════════════════════════

┌─────────────────────────────────────────────────────────────────────────────┐
│ scripts/ptq/evaluators/evaluator.py                                          │
│ ┌─────────────────────────────────────────────────────────────────────────┐ │
│ │ PTQEvaluator.evaluate(self, model_name, device, tasks, ...)             │ │
│ │   │                                                                      │ │
│ │   ├── 【创建量化配置】                                                    │ │
│ │   │   └──▶ quantization_config = ModelQuantizationConfig(               │ │
│ │   │              activation_scale_rule=ScaleRule.static_6,              │ │
│ │   │              dtype=DataType.nvfp4,                                  │ │
│ │   │              weight_scale_rule=ScaleRule.static_6,                  │ │
│ │   │          )                                                           │ │
│ │   │          │                                                           │ │
│ │   │          │  src/fouroversix/model/config.py                          │ │
│ │   │          │  ┌────────────────────────────────────────────────────┐   │ │
│ │   │          └──│ ModelQuantizationConfig.__post_init__()            │   │ │
│ │   │             │   └── 转换字符串参数为枚举类型                      │   │ │
│ │   │             └────────────────────────────────────────────────────┘   │ │
│ │   │                                                                      │ │
│ │   ├── 【量化模型】                                                        │ │
│ │   │   └──▶ model = self.quantize_model(                                 │ │
│ │   │              model_name, device, save_path, quantization_config     │ │
│ │   │          )                                                           │ │
│ │   │          │                                                           │ │
│ │   │          ▼                                                           │ │
│ │   │   ┌──────────────────────────────────────────────────────────────┐   │ │
│ │   │   │ scripts/ptq/evaluators/rtn.py                                 │   │ │
│ │   │   │ ┌────────────────────────────────────────────────────────────┐│   │ │
│ │   │   │ │ RTNEvaluatorImpl.quantize_model(self, model_name, ...)     ││   │ │
│ │   │   │ │   │                                                        ││   │ │
│ │   │   │ │   ├── 【创建HF量化配置】                                    ││   │ │
│ │   │   │ │   │   └──▶ hf_quantization_config = HFFourOverSixConfig(   ││   │ │
│ │   │   │ │   │          activation_scale_rule=ScaleRule.static_6,     ││   │ │
│ │   │   │ │   │          dtype=DataType.nvfp4,                         ││   │ │
│ │   │   │ │   │          weight_scale_rule=ScaleRule.static_6,         ││   │ │
│ │   │   │ │   │      )                                                 ││   │ │
│ │   │   │ │   │                                                        ││   │ │
│ │   │   │ │   └──▶ 【加载模型并量化】                                   ││   │ │
│ │   │   │ │       model = AutoModelForCausalLM.from_pretrained(        ││   │ │
│ │   │   │ │           model_name,                                      ││   │ │
│ │   │   │ │           device_map=device,                               ││   │ │
│ │   │   │ │           quantization_config=hf_quantization_config,      ││   │ │
│ │   │   │ │       )                                                    ││   │ │
│ │   │   │ │       │                                                    ││   │ │
│ │   │   │ │       ▼                                                    ││   │ │
│ │   │   │ │   ┌────────────────────────────────────────────────────┐   ││   │ │
│ │   │   │ │   │ 【详见阶段5: 模型加载与量化流程】                   │   ││   │ │
│ │   │   │ │   └────────────────────────────────────────────────────┘   ││   │ │
│ │   │   │ └────────────────────────────────────────────────────────────┘│   │ │
│ │   │   └──────────────────────────────────────────────────────────────┘   │ │
│ │   │                                                                      │ │
│ │   ├── 【运行评估任务】                                                    │ │
│ │   │   └──▶ full_results = evaluator.simple_evaluate(                    │ │
│ │   │              model=HFLM(pretrained=model, device=device),           │ │
│ │   │              tasks=tasks,  # ["wikitext"]                           │ │
│ │   │          )                                                           │ │
│ │   │          │                                                           │ │
│ │   │          │  lm_eval库 (外部依赖)                                      │ │
│ │   │          │  ┌────────────────────────────────────────────────────┐   │ │
│ │   │          └──│ evaluator.simple_evaluate()                        │   │ │
│ │   │             │   ├── 加载任务数据集                                 │   │ │
│ │   │             │   ├── 对每个任务运行模型推理                         │   │ │
│ │   │             │   │   └──▶ model.generate() / model.forward()      │   │ │
│ │   │             │   │       │                                         │   │ │
│ │   │             │   │       ▼                                         │   │ │
│ │   │             │   │   ┌──────────────────────────────────────────┐ │   │ │
│ │   │             │   │   │ 【详见阶段6: 模型推理流程】              │ │   │ │
│ │   │             │   │   └──────────────────────────────────────────┘ │   │ │
│ │   │             │   └── 计算评估指标                                   │   │ │
│ │   │             └────────────────────────────────────────────────────┘   │ │
│ │   │                                                                      │ │
│ │   └── return results                                                     │ │
│ └─────────────────────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
═══════════════════════════════════════════════════════════════════════════════
【阶段5: 模型加载与量化流程】
═══════════════════════════════════════════════════════════════════════════════

┌─────────────────────────────────────────────────────────────────────────────┐
│ transformers库 (AutoModelForCausalLM.from_pretrained)                        │
│ ┌─────────────────────────────────────────────────────────────────────────┐ │
│ │ 当加载带有HFFourOverSixConfig的模型时，会自动触发量化流程:               │ │
│ │                                                                          │ │
│ │ AutoModelForCausalLM.from_pretrained(model_name, quantization_config)   │ │
│ │   │                                                                      │ │
│ │   ├── 加载模型架构和权重                                                 │ │
│ │   │                                                                      │ │
│ │   └──▶ 【应用量化配置】                                                  │ │
│ │       └──▶ quantize_model(model, config)                                │ │
│ │              │                                                           │ │
│ │              │  src/fouroversix/model/quantize.py                         │ │
│ │              │  ┌────────────────────────────────────────────────────┐    │ │
│ │              └──│ quantize_model(model, config, **kwargs)           │    │ │
│ │                 │   │                                                │    │ │
│ │                 │   └── for module_name, module in model.named_modules():│ │
│ │                 │       │                                            │    │ │
│ │                 │       ├── 跳过: module_name == ""                  │    │ │
│ │                 │       ├── 跳过: module_name in modules_to_not_convert│   │ │
│ │                 │       │                                            │    │ │
│ │                 │       └──▶ 【替换模块】                             │    │ │
│ │                 │           module_cls = QuantizedModule.get_cls(    │    │ │
│ │                 │               type(module)                         │    │ │
│ │                 │           )  # 返回 FourOverSixLinear              │    │ │
│ │                 │           │                                        │    │ │
│ │                 │           │  src/fouroversix/model/quantize.py      │    │ │
│ │                 │           │  ┌────────────────────────────────────┐│    │ │
│ │                 │           └──│ QuantizedModule.get_cls(nn.Linear)││    │ │
│ │                 │              │   return _registry[nn.Linear]      ││    │ │
│ │                 │              │   # 返回 FourOverSixLinear         ││    │ │
│ │                 │              └────────────────────────────────────┘│    │ │
│ │                 │                                                      │    │ │
│ │                 │           quantized_module = module_cls(            │    │ │
│ │                 │               module,                               │    │ │
│ │                 │               config.get_module_config(module_name) │    │ │
│ │                 │           )                                          │    │ │
│ │                 │           │                                          │    │ │
│ │                 │           ▼                                          │    │ │
│ │                 │   ┌──────────────────────────────────────────────┐  │    │ │
│ │                 │   │ 【详见阶段5.1: FourOverSixLinear初始化】     │  │    │ │
│ │                 │   └──────────────────────────────────────────────┘  │    │ │
│ │                 │                                                      │    │ │
│ │                 │           model.set_submodule(module_name, quantized_module)│ │
│ │                 └────────────────────────────────────────────────────┘    │ │
│ └─────────────────────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│ 【阶段5.1: FourOverSixLinear初始化】                                          │
│                                                                              │
│ src/fouroversix/model/modules/linear.py                                      │
│ ┌─────────────────────────────────────────────────────────────────────────┐ │
│ │ FourOverSixLinear.__init__(self, module, config)                        │ │
│ │   │                                                                      │ │
│ │   ├── super().__init__(in_features, out_features, ...)                  │ │
│ │   ├── self.weight = module.weight  ← 保存原始权重                       │ │
│ │   ├── self.bias = module.bias                                            │ │
│ │   ├── self.config = config                                               │ │
│ │   │                                                                      │ │
│ │   └──▶ 【量化权重】                                                      │ │
│ │       quantized_params = self.get_quantized_parameters(                 │ │
│ │           "weight", self.weight                                          │ │
│ │       )                                                                  │ │
│ │       │                                                                  │ │
│ │       │  src/fouroversix/model/modules/linear.py                         │ │
│ │       │  ┌────────────────────────────────────────────────────────────┐  │ │
│ │       └──│ get_quantized_parameters(self, parameter_name, parameter) │  │ │
│ │          │   if parameter_name == "weight":                          │  │ │
│ │          │       config = QuantizationConfig(                        │  │ │
│ │          │           dtype=self.config.dtype,  # nvfp4               │  │ │
│ │          │           scale_rule=self.config.get_weight_scale_rule(), │  │ │
│ │          │           # scale_rule = static_6                         │  │ │
│ │          │       )                                                   │  │ │
│ │          │       │                                                   │  │ │
│ │          │       └──▶ quantized_weight = quantize_to_fp4(            │  │ │
│ │          │               parameter, config                           │  │ │
│ │          │           )                                               │  │ │
│ │          │               │                                           │  │ │
│ │          │               ▼                                           │  │ │
│ │          │   ┌──────────────────────────────────────────────────┐    │  │ │
│ │          │   │ 【详见阶段5.2: quantize_to_fp4量化流程】         │    │  │ │
│ │          │   └──────────────────────────────────────────────────┘    │  │ │
│ │          │                                                           │  │ │
│ │          │   return {                                                │  │ │
│ │          │       "quantized_weight_values": quantized_weight.values, │  │ │
│ │          │       "quantized_weight_scale_factors": quantized_weight.scale_factors,│ │ │
│ │          │       "quantized_weight_amax": quantized_weight.amax,     │  │ │
│ │          │   }                                                       │  │ │
│ │          └────────────────────────────────────────────────────────────┘  │ │
│ │                                                                          │ │
│ │   ├── self.register_buffer("quantized_weight_values", ...)              │ │
│ │   ├── self.register_buffer("quantized_weight_scale_factors", ...)       │ │
│ │   └── self.register_buffer("quantized_weight_amax", ...)                │ │
│ └─────────────────────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│ 【阶段5.2: quantize_to_fp4量化流程】                                          │
│                                                                              │
│ src/fouroversix/quantize/frontend.py                                         │
│ ┌─────────────────────────────────────────────────────────────────────────┐ │
│ │ quantize_to_fp4(x, config)                                              │ │
│ │   │                                                                      │ │
│ │   ├── if config.backend is None:                                        │ │
│ │   │       【自动选择后端】                                               │ │
│ │   │       for backend in [cuda, triton, pytorch]:                       │ │
│ │   │           if AVAILABLE_BACKENDS[backend].is_supported(x, config):   │ │
│ │   │               selected_backend = backend                            │ │
│ │   │               break                                                  │ │
│ │   │       # 假设选择 pytorch (无Blackwell GPU时)                        │ │
│ │   │                                                                      │ │
│ │   └──▶ return AVAILABLE_BACKENDS[selected_backend].quantize_to_fp4(x, config)│ │
│ │              │                                                           │ │
│ │              ▼                                                           │ │
│ │   ┌──────────────────────────────────────────────────────────────────┐   │ │
│ │   │ src/fouroversix/quantize/pytorch/backend.py                       │   │ │
│ │   │ ┌────────────────────────────────────────────────────────────────┐│   │ │
│ │   │ │ PyTorchQuantizeBackend.quantize_to_fp4(x, config)              ││   │ │
│ │   │ │   │                                                            ││   │ │
│ │   │ │   ├── 【填充张量】                                              ││   │ │
│ │   │ │   │   x = F.pad(x, ...)  # 填充到128x64的倍数                  ││   │ │
│ │   │ │   │                                                            ││   │ │
│ │   │ │   └──▶ values, scale_factors, amax = quantize_to_fp4(         ││   │ │
│ │   │ │           x,                                                   ││   │ │
│ │   │ │           had=None,  # 不使用RHT                               ││   │ │
│ │   │ │           fp4_format=DataType.nvfp4,                           ││   │ │
│ │   │ │           round_style=RoundStyle.nearest,                      ││   │ │
│ │   │ │           scale_rule=ScaleRule.static_6,  # 标准NVFP4          ││   │ │
│ │   │ │           block_scale_2d=False,                                ││   │ │
│ │   │ │           transpose=False,                                     ││   │ │
│ │   │ │       )                                                        ││   │ │
│ │   │ │       │                                                        ││   │ │
│ │   │ │       ▼                                                        ││   │ │
│ │   │ │   ┌──────────────────────────────────────────────────────────┐││   │ │
│ │   │ │   │ 【详见阶段5.3: 核心量化函数】                             │││   │ │
│ │   │ │   └──────────────────────────────────────────────────────────┘││   │ │
│ │   │ │                                                                ││   │ │
│ │   │ │   return QuantizedTensor(values, scale_factors, amax, ...)    ││   │ │
│ │   │ └────────────────────────────────────────────────────────────────┘│   │ │
│ │   └──────────────────────────────────────────────────────────────────┘   │ │
│ └─────────────────────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│ 【阶段5.3: 核心量化函数 quantize_to_fp4】                                      │
│                                                                              │
│ src/fouroversix/quantize/pytorch/reference.py                                │
│ ┌─────────────────────────────────────────────────────────────────────────┐ │
│ │ quantize_to_fp4(x, x_amax, had, fp4_format, round_style, scale_rule, ...)│ │
│ │   │                                                                      │ │
│ │   ├── 【1. 可选: 应用随机Hadamard变换】                                  │ │
│ │   │   if had is not None:                                               │ │
│ │   │       x = (x.reshape(-1, had.shape[0]) @ had).reshape_as(x)         │ │
│ │   │   # 本例中 had=None，跳过                                           │ │
│ │   │                                                                      │ │
│ │   ├── 【2. 计算全局最大值】                                              │ │
│ │   │   if x_amax is None:                                                │ │
│ │   │       x_amax = x.abs().max().float()                                │ │
│ │   │                                                                      │ │
│ │   ├── 【3. 分块】                                                        │ │
│ │   │   x_scale_blocks = x.reshape(-1, fp4_format.block_size()).float()   │ │
│ │   │   # block_size = 16 for NVFP4                                       │ │
│ │   │   # x_scale_blocks.shape = [num_blocks, 16]                         │ │
│ │   │                                                                      │ │
│ │   ├── 【4. 根据scale_rule选择量化方法】                                  │ │
│ │   │   if scale_rule in {ScaleRule.static_6, ScaleRule.static_4}:        │ │
│ │   │       # 标准NVFP4量化                                                │ │
│ │   │       └──▶ x_block_scaled, scales = quantize_to_nvfp4(              │ │
│ │   │               x_scale_blocks, x_amax, scale_rule=scale_rule          │ │
│ │   │           )                                                          │ │
│ │   │           │                                                          │ │
│ │   │           ▼                                                          │ │
│ │   │   ┌──────────────────────────────────────────────────────────────┐  │ │
│ │   │   │ 【详见阶段5.4: quantize_to_nvfp4】                            │  │ │
│ │   │   └──────────────────────────────────────────────────────────────┘  │ │
│ │   │                                                                      │ │
│ │   ├── 【5. 伪量化到E2M1】                                                │ │
│ │   │   x_fake_quantized = fake_quantize_to_e2m1(                         │ │
│ │   │       x_block_scaled, round_style=round_style                       │ │
│ │   │   )                                                                  │ │
│ │   │   │                                                                  │ │
│ │   │   │  src/fouroversix/quantize/pytorch/reference.py                   │ │
│ │   │   │  ┌────────────────────────────────────────────────────────────┐  │ │
│ │   │   └──│ fake_quantize_to_e2m1(x, round_style)                     │  │ │
│ │   │      │   # 将浮点数量化为E2M1格式的伪量化函数                      │  │ │
│ │   │      │   # E2M1可表示: 0, 0.5, 1, 1.5, 2, 3, 4, 6                  │  │ │
│ │   │      │   step1 = round(2 * |x|) / 2    # |x| < 2                  │  │ │
│ │   │      │   step2 = round(|x|)            # 2 <= |x| < 4             │  │ │
│ │   │      │   step3 = 2 * round(|x| / 2)    # |x| >= 4                 │  │ │
│ │   │      │   return sign(x) * (step1*mask1 + step2*mask2 + step3*mask3)│  │ │
│ │   │      └────────────────────────────────────────────────────────────┘  │ │
│ │   │                                                                      │ │
│ │   ├── 【6. 打包为uint8】                                                 │ │
│ │   │   x_quantized = pack_unpacked_fp4(                                  │ │
│ │   │       quantize_bf16_to_unpacked_fp4(                                │ │
│ │   │           x_fake_quantized.bfloat16().reshape_as(x)                 │ │
│ │   │       )                                                              │ │
│ │   │   )                                                                  │ │
│ │   │   # 每个uint8存储2个E2M1值                                          │ │
│ │   │                                                                      │ │
│ │   ├── 【7. 重新排列缩放因子】                                            │ │
│ │   │   reshaped_scales = to_blocked(                                     │ │
│ │   │       scales.reshape(x.shape[0], x.shape[1] // 16)                  │ │
│ │   │   )                                                                  │ │
│ │   │                                                                      │ │
│ │   └── return x_quantized, reshaped_scales, x_amax                       │ │
│ └─────────────────────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│ 【阶段5.4: quantize_to_nvfp4核心量化】                                        │
│                                                                              │
│ src/fouroversix/quantize/pytorch/reference.py                                │
│ ┌─────────────────────────────────────────────────────────────────────────┐ │
│ │ quantize_to_nvfp4(x_scale_blocks, x_amax, scale_rule, ...)              │ │
│ │   │                                                                      │ │
│ │   ├── if x_amax == 0:                                                   │ │
│ │   │       x_scales_hp = zeros(...)                                      │ │
│ │   │   else:                                                             │ │
│ │   │       【计算编码缩放因子】                                           │ │
│ │   │       encode_scale = (                                               │ │
│ │   │           scale_rule.max_allowed_e2m1_value()  # 6                  │ │
│ │   │           * scale_rule.max_allowed_e4m3_value()  # 448              │ │
│ │   │           / x_amax                                                   │ │
│ │   │       )                                                              │ │
│ │   │                                                                      │ │
│ │   │       【计算每块的缩放因子】                                         │ │
│ │   │       x_scales_hp = (                                                │ │
│ │   │           x_scale_blocks.abs().max(axis=-1).values                  │ │
│ │   │           / 6  # max_allowed_e2m1_value                             │ │
│ │   │           * encode_scale                                             │ │
│ │   │       )                                                              │ │
│ │   │                                                                      │ │
│ │   ├── 【转换为E4M3格式】                                                 │ │
│ │   │   x_scales = x_scales_hp.to(torch.float8_e4m3fn)                    │ │
│ │   │                                                                      │ │
│ │   ├── 【计算解码缩放因子】                                               │ │
│ │   │   decode_scale = 1 / (6 * 448 / x_amax)                             │ │
│ │   │                                                                      │ │
│ │   ├── 【缩放数据块】                                                     │ │
│ │   │   x_block_scaled = torch.where(                                     │ │
│ │   │       x_scales.unsqueeze(1) != 0,                                   │ │
│ │   │       x_scale_blocks * (1 / (decode_scale * x_scales.unsqueeze(1))),│ │
│ │   │       0,                                                            │ │
│ │   │   )                                                                  │ │
│ │   │                                                                      │ │
│ │   └── return x_block_scaled, x_scales                                   │ │
│ └─────────────────────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
═══════════════════════════════════════════════════════════════════════════════
【阶段6: 模型推理流程】
═══════════════════════════════════════════════════════════════════════════════

┌─────────────────────────────────────────────────────────────────────────────┐
│ lm_eval库调用模型推理时:                                                     │
│                                                                              │
│ model(input_ids)  或  model.generate(input_ids)                             │
│   │                                                                          │ │
│   └──▶ 遍历模型的每一层                                                      │ │
│         └──▶ 每个FourOverSixLinear层调用forward()                           │ │
│                │                                                             │ │
│                ▼                                                             │ │
│ ┌─────────────────────────────────────────────────────────────────────────┐ │
│ │ src/fouroversix/model/modules/linear.py                                  │ │
│ │ ┌─────────────────────────────────────────────────────────────────────┐ │ │
│ │ │ FourOverSixLinear.forward(self, input)                              │ │ │
│ │ │   │                                                                 │ │ │
│ │ │   └──▶ return FourOverSixLinearFunction.apply(                      │ │ │
│ │ │           self.config,                                              │ │ │
│ │ │           input,                                                    │ │ │
│ │ │           self.quantized_weight(),  # QuantizedTensor               │ │ │
│ │ │           self.bias,                                                │ │ │
│ │ │       )                                                             │ │ │
│ │ │       │                                                             │ │ │
│ │ │       ▼                                                             │ │ │
│ │ │   ┌───────────────────────────────────────────────────────────────┐ │ │ │
│ │ │   │ FourOverSixLinearFunction.forward(ctx, config, input, weight)│ │ │ │
│ │ │   │   │                                                          │ │ │ │
│ │ │   │   ├── 【获取量化配置】                                        │ │ │ │
│ │ │   │   │   fprop_activation_config = config.get_activation_config()│ │ │ │
│ │ │   │   │   # scale_rule = static_6                                │ │ │ │
│ │ │   │   │                                                          │ │ │ │
│ │ │   │   ├── 【保存用于反向传播的张量】                              │ │ │ │
│ │ │   │   │   ctx.save_for_backward(input, weight, bias)             │ │ │ │
│ │ │   │   │                                                          │ │ │ │
│ │ │   │   └──▶ 【执行FP4矩阵乘法】                                   │ │ │ │
│ │ │   │       out = fp4_matmul(                                      │ │ │ │
│ │ │   │           input.reshape(-1, input.shape[-1]),                │ │ │ │
│ │ │   │           weight,  # QuantizedTensor                          │ │ │ │
│ │ │   │           backend=config.matmul_backend,                     │ │ │ │
│ │ │   │           input_config=fprop_activation_config,              │ │ │ │
│ │ │   │           out_dtype=config.output_dtype,  # bfloat16         │ │ │ │
│ │ │   │       )                                                      │ │ │ │
│ │ │   │       │                                                      │ │ │ │
│ │ │   │       ▼                                                      │ │ │ │
│ │ │   │   ┌────────────────────────────────────────────────────────┐ │ │ │ │
│ │ │   │   │ 【详见阶段6.1: fp4_matmul流程】                        │ │ │ │ │
│ │ │   │   └────────────────────────────────────────────────────────┘ │ │ │ │
│ │ │   │                                                              │ │ │ │
│ │ │   │   if bias is not None:                                       │ │ │ │
│ │ │   │       out = out + bias                                       │ │ │ │
│ │ │   │                                                              │ │ │ │
│ │ │   │   return out                                                 │ │ │ │
│ │ │   └──────────────────────────────────────────────────────────────┘ │ │ │
│ │ └─────────────────────────────────────────────────────────────────────┘ │ │
│ └─────────────────────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│ 【阶段6.1: fp4_matmul流程】                                                   │
│                                                                              │
│ src/fouroversix/matmul/frontend.py                                           │
│ ┌─────────────────────────────────────────────────────────────────────────┐ │
│ │ fp4_matmul(input, other, backend, input_config, out_dtype, ...)         │ │
│ │   │                                                                      │ │
│ │   ├── 【量化高精度输入】                                                 │ │
│ │   │   if isinstance(input, torch.Tensor):                               │ │
│ │   │       └──▶ input = quantize_to_fp4(input, input_config)             │ │
│ │   │              # 对激活值进行量化                                      │ │
│ │   │              # 流程同阶段5.2-5.4                                     │ │
│ │   │                                                                      │ │
│ │   ├── 【选择后端】                                                       │ │
│ │   │   if backend is None:                                               │ │
│ │   │       for backend_candidate in [cutlass, pytorch]:                  │ │
│ │   │           if is_supported(...):                                     │ │
│ │   │               backend = backend_candidate                           │ │
│ │   │               break                                                  │ │
│ │   │       # 假设选择 pytorch (无Blackwell GPU时)                        │ │
│ │   │                                                                      │ │
│ │   └──▶ return AVAILABLE_BACKENDS[backend].fp4_matmul(                   │ │
│ │              input, other, out_dtype=out_dtype                          │ │
│ │          )                                                               │ │
│ │          │                                                               │ │
│ │          ▼                                                               │ │
│ │   ┌──────────────────────────────────────────────────────────────────┐   │ │
│ │   │ src/fouroversix/matmul/pytorch.py                                  │   │ │
│ │   │ ┌────────────────────────────────────────────────────────────────┐│   │ │
│ │   │ │ PyTorchMatmulBackend.fp4_matmul(input, other, out_dtype)       ││   │ │
│ │   │ │   │                                                            ││   │ │
│ │   │ │   ├── 【反量化输入】                                            ││   │ │
│ │   │ │   │   input_dequantized = input.dequantize(dtype=torch.float32)││   │ │
│ │   │ │   │   │                                                        ││   │ │
│ │   │ │   │   │  src/fouroversix/quantize/quantized_tensor.py           ││   │ │
│ │   │ │   │   │  ┌────────────────────────────────────────────────────┐││   │ │
│ │   │ │   │   └──│ QuantizedTensor.dequantize(dtype)                  │││   │ │
│ │   │ │   │      │   values = unpack_packed_fp4(self.values)          │││   │ │
│ │   │ │   │      │   scales = from_blocked(self.scale_factors, ...)   │││   │ │
│ │   │ │   │      │   result = values * scales * amax / (6 * 448)      │││   │ │
│ │   │ │   │      │   return result                                     │││   │ │
│ │   │ │   │      └────────────────────────────────────────────────────┘││   │ │
│ │   │ │   │                                                            ││   │ │
│ │   │ │   ├── 【反量化权重】                                            ││   │ │
│ │   │ │   │   other_dequantized = other.dequantize(dtype=torch.float32)││   │ │
│ │   │ │   │                                                            ││   │ │
│ │   │ │   └──▶ 【执行矩阵乘法】                                         ││   │ │
│ │   │ │       out = torch.matmul(                                      ││   │ │
│ │   │ │           input_dequantized,                                   ││   │ │
│ │   │ │           other_dequantized.T                                  ││   │ │
│ │   │ │       ).to(out_dtype.torch_dtype())  # bfloat16               ││   │ │
│ │   │ │       return out                                               ││   │ │
│ │   │ └────────────────────────────────────────────────────────────────┘│   │ │
│ │   └──────────────────────────────────────────────────────────────────┘   │ │
│ └─────────────────────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
═══════════════════════════════════════════════════════════════════════════════
【阶段7: 结果保存】
═══════════════════════════════════════════════════════════════════════════════

┌─────────────────────────────────────────────────────────────────────────────┐
│ scripts/ptq/coordinators/base.py                                             │
│ ┌─────────────────────────────────────────────────────────────────────────┐ │
│ │ BaseEvaluationCoordinator.save_results(self, model_name, ptq_method, ...)│ │
│ │   │                                                                      │ │
│ │   ├── session = self.get_session()                                      │ │
│ │   │   └── 创建SQLite数据库连接                                          │ │
│ │   │                                                                      │ │
│ │   └── for task, metric_name, metric_value, full_results in results:     │ │
│ │         experiment = Experiment(                                        │ │
│ │             group_name=self.group_name,                                 │ │
│ │             model_name=model_name,                                      │ │
│ │             task=task,                                                  │ │
│ │             metric_name=metric_name,  # "word_perplexity,none"          │ │
│ │             metric_value=metric_value,                                  │ │
│ │             ptq_method=ptq_method.value,  # "rtn"                       │ │
│ │             activation_scale_rule="static_6",                           │ │
│ │             weight_scale_rule="static_6",                               │ │
│ │             results=full_results,                                       │ │
│ │         )                                                               │ │
│ │         session.add(experiment)                                         │ │
│ │                                                                          │ │
│ │   session.commit()                                                       │ │
│ └─────────────────────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 1.7.4 详细执行流程

#### 步骤1: 命令行入口

**文件位置**: `scripts/ptq/__main__.py`

```python
@click.command()
@click.option("--model-name", "-m", type=str, multiple=True, required=True)
@click.option("--ptq-method", "-p", type=PTQMethod, multiple=True, required=True)
@click.option("--task", "-t", type=str, multiple=True, default=["wikitext"])
@click.option("--activation-scale-rule", "--a-scale-rule", type=ScaleRule, default=ScaleRule.mse)
@click.option("--weight-scale-rule", "--w-scale-rule", type=ScaleRule, default=ScaleRule.mse)
def cli(**kwargs: dict[str, Any]) -> None:
    # 解析参数
    model_names = kwargs.pop("model_name")      # ["meta-llama/Llama-3.2-1B"]
    ptq_methods = kwargs.pop("ptq_method")      # [PTQMethod.rtn]
    tasks = kwargs.pop("task")                  # ["wikitext"]
    use_modal = kwargs.pop("modal")             # False (本地测试)
    
    # 创建协调器并启动
    coordinator = LocalEvaluationCoordinator(group_name)
    coordinator.start(model_names, ptq_methods, tasks, **kwargs)
```

**类/函数说明**:
- `cli()`: Click命令行入口函数，解析命令行参数
- `PTQMethod`: 枚举类，定义支持的PTQ方法（rtn, awq, gptq等）
- `ScaleRule`: 枚举类，定义缩放规则（static_6, static_4, mse等）

---

#### 步骤2: 创建本地协调器

**文件位置**: `scripts/ptq/coordinators/local.py`

```python
class LocalEvaluationCoordinator(BaseEvaluationCoordinator):
    def __init__(self, group_name: str | None = None) -> None:
        self.database_path = FOUROVERSIX_ROOT_DIR / "results.db"  # 结果数据库路径
        self.group_name = group_name
```

**类说明**:
- `LocalEvaluationCoordinator`: 本地测试协调器，继承自`BaseEvaluationCoordinator`
- 负责管理本地多进程测试、任务分发、结果收集

---

#### 步骤3: 启动多进程Worker

**文件位置**: `scripts/ptq/coordinators/local.py`

```python
def start(
    self,
    model_names: list[str],
    ptq_methods: list[PTQMethod],
    tasks: list[str],
    *,
    device: str,
    **kwargs: dict[str, Any],
) -> None:
    multiprocessing.set_start_method("spawn", force=True)
    
    manager = multiprocessing.Manager()
    task_queue = manager.Queue()      # 任务队列
    result_queue = manager.Queue()    # 结果队列
    
    # 为每个GPU启动一个Worker进程
    num_workers = torch.cuda.device_count() if device == "cuda" else 1
    workers = []
    for gpu_id in range(num_workers):
        p = multiprocessing.Process(
            target=self.worker,
            args=(f"cuda:{gpu_id}" if device == "cuda" else device, task_queue, result_queue),
        )
        p.start()
        workers.append(p)
    
    # ... 分发任务 ...
    
    # 收集结果
    for _ in range(experiments):
        self.save_results(*result_queue.get())
```

**函数说明**:
- `start()`: 启动测试流程的主函数
- 使用Python多进程实现并行测试
- 每个GPU对应一个独立的Worker进程

---

#### 步骤4: Worker进程执行

**文件位置**: `scripts/ptq/coordinators/local.py`

```python
def worker(
    self,
    device: str,
    task_queue: multiprocessing.Queue,
    result_queue: multiprocessing.Queue,
) -> None:
    while True:
        worker_task = task_queue.get()
        if worker_task is None:
            break
        
        model_name, ptq_method, kwargs = worker_task
        
        results = self.evaluate(
            model_name,
            ptq_method,
            **{**kwargs, "device": device},
        )
        
        result_queue.put((model_name, ptq_method, kwargs, results))
```

**函数说明**:
- `worker()`: Worker进程的主循环
- 从任务队列获取任务，执行评估，将结果放入结果队列

---

#### 步骤5: 获取评估器并执行评估

**文件位置**: `scripts/ptq/coordinators/local.py` → `scripts/ptq/evaluators/__init__.py`

```python
# local.py
def evaluate(self, model_name: str, ptq_method: PTQMethod, **kwargs) -> dict[str, Any]:
    evaluator_cls = get_evaluator(ptq_method)  # 获取RTNEvaluator类
    return evaluator_cls().evaluate(model_name=model_name, save_path=..., **kwargs)

# evaluators/__init__.py
def get_evaluator(ptq_method: PTQMethod) -> type[PTQEvaluator]:
    if ptq_method == PTQMethod.rtn:
        return RTNEvaluator
    # ... 其他方法 ...
```

**类说明**:
- `get_evaluator()`: 工厂函数，根据PTQ方法返回对应的评估器类
- `RTNEvaluator`: Round-to-Nearest量化评估器

---

#### 步骤6: RTNEvaluator评估流程

**文件位置**: `scripts/ptq/evaluators/rtn.py` → `scripts/ptq/evaluators/evaluator.py`

```python
# rtn.py
class RTNEvaluatorImpl(PTQEvaluator):
    def quantize_model(
        self,
        model_name: str,
        *,
        device: str,
        save_path: Path,
        quantization_config: ModelQuantizationConfig,
        trust_remote_code: bool = False,
    ) -> AutoModelForCausalLM:
        # 创建HuggingFace量化配置
        hf_quantization_config = HFFourOverSixConfig(
            activation_scale_rule=quantization_config.get_activation_scale_rule(),  # static_6
            dtype=quantization_config.dtype,                                         # nvfp4
            weight_scale_rule=quantization_config.get_weight_scale_rule(),          # static_6
            # ... 其他配置 ...
        )
        
        # 加载模型并自动量化
        model = AutoModelForCausalLM.from_pretrained(
            model_name,
            device_map=device,
            quantization_config=hf_quantization_config,
            trust_remote_code=trust_remote_code,
        )
        
        return model

# evaluator.py
class PTQEvaluator(ABC):
    def evaluate(
        self,
        model_name: str,
        *,
        device: str,
        dtype: str,
        eval_framework: EvaluationFramework,
        tasks: list[str],
        **kwargs: dict[str, Any],
    ) -> dict[str, Any]:
        # 1. 创建量化配置
        quantization_config = ModelQuantizationConfig(
            activation_scale_rule=activation_scale_rule,  # static_6
            dtype=dtype,                                  # nvfp4
            weight_scale_rule=weight_scale_rule,          # static_6
        )
        
        # 2. 量化模型
        model = self.quantize_model(
            model_name=model_name,
            device=device,
            quantization_config=quantization_config,
            **kwargs,
        )
        
        # 3. 运行评估
        if eval_framework == EvaluationFramework.lm_eval:
            from lm_eval import evaluator
            full_results = evaluator.simple_evaluate(
                model=HFLM(pretrained=model, device=device),
                tasks=tasks,  # ["wikitext"]
            )
        
        return results
```

**类说明**:
- `RTNEvaluatorImpl`: RTN量化评估器实现类
- `PTQEvaluator`: 评估器抽象基类，定义评估流程
- `HFFourOverSixConfig`: HuggingFace格式的FourOverSix量化配置
- `ModelQuantizationConfig`: 模型级量化配置类

---

#### 步骤7: 模型加载与自动量化

**文件位置**: `src/fouroversix/model/quantize.py`

当`AutoModelForCausalLM.from_pretrained()`加载带有`HFFourOverSixConfig`的模型时，会自动触发量化流程：

```python
def quantize_model(
    model: nn.Module,
    config: ModelQuantizationConfig,
    **kwargs: dict[str, Any],
) -> None:
    """
    遍历模型中的所有模块，将nn.Linear替换为FourOverSixLinear
    """
    for module_name, module in model.named_modules():
        if (
            module_name == ""
            or module_name in config.modules_to_not_convert  # 跳过lm_head
            or not isinstance(module, nn.Module)
        ):
            continue
        
        # 获取量化后的模块类
        module_cls = QuantizedModule.get_cls(type(module))
        should_replace = QuantizedModule.should_replace_existing_modules_in_model(type(module))
        
        if module_cls is None or not should_replace:
            continue
        
        # 创建量化模块并替换
        quantized_module = module_cls(module, config.get_module_config(module_name), **kwargs)
        model.set_submodule(module_name, quantized_module)
```

**函数说明**:
- `quantize_model()`: 模型量化主函数，遍历并替换所有可量化模块
- `QuantizedModule`: 量化模块的注册和管理类

---

#### 步骤8: 线性层替换

**文件位置**: `src/fouroversix/model/modules/linear.py`

```python
@QuantizedModule.register(nn.Linear)
class FourOverSixLinear(nn.Linear):
    """
    nn.Linear的量化替代类
    """
    
    def __init__(
        self,
        module: nn.Linear,
        config: ModuleQuantizationConfig,
    ) -> None:
        # 继承原始线性层的参数
        super().__init__(
            module.in_features,
            module.out_features,
            module.bias is not None,
            module.weight.device,
            module.weight.dtype,
        )
        
        self.weight = module.weight
        self.bias = module.bias
        self.config = config
        
        # 注册量化后的权重buffer
        if not self.config.keep_master_weights:
            self.register_buffer("quantized_weight_values", ...)
            self.register_buffer("quantized_weight_scale_factors", ...)
            self.register_buffer("quantized_weight_amax", ...)
    
    def get_quantized_parameters(self, parameter_name: str, parameter: torch.Tensor) -> dict[str, Any]:
        """量化权重参数"""
        if parameter_name == "weight":
            config = QuantizationConfig(
                backend=self.config.quantize_backend,
                dtype=self.config.dtype,                    # nvfp4
                scale_rule=self.config.get_weight_scale_rule(),  # static_6
            )
            quantized_weight = quantize_to_fp4(parameter, config)
            return {
                "quantized_weight_values": quantized_weight.values,
                "quantized_weight_scale_factors": quantized_weight.scale_factors,
                "quantized_weight_amax": quantized_weight.amax,
            }
    
    def forward(self, input: torch.Tensor) -> torch.Tensor:
        """前向传播，使用FP4矩阵乘法"""
        return FourOverSixLinearFunction.apply(
            self.config,
            input,
            self.quantized_weight(),
            self.bias,
        )
```

**类说明**:
- `FourOverSixLinear`: 量化线性层类，使用装饰器`@QuantizedModule.register(nn.Linear)`注册
- 替换原始的`nn.Linear`，在forward时使用FP4矩阵乘法

---

#### 步骤9: 权重量化

**文件位置**: `src/fouroversix/quantize/frontend.py` → `src/fouroversix/quantize/pytorch/backend.py` → `src/fouroversix/quantize/pytorch/reference.py`

```python
# frontend.py
def quantize_to_fp4(
    x: torch.Tensor,
    config: QuantizationConfig | None = None,
) -> QuantizedTensor:
    """量化张量到FP4的主入口"""
    if config is None:
        config = QuantizationConfig()
    
    selected_backend = config.backend
    
    # 自动选择后端
    if selected_backend is None:
        for backend in [QuantizeBackend.cuda, QuantizeBackend.triton, QuantizeBackend.pytorch]:
            if AVAILABLE_BACKENDS[backend].is_supported(x, config):
                selected_backend = backend
                break
    
    return AVAILABLE_BACKENDS[selected_backend].quantize_to_fp4(x, config)

# pytorch/backend.py
class PyTorchQuantizeBackend(QuantizeBackendBase):
    @classmethod
    def quantize_to_fp4(cls, x: torch.Tensor, config: QuantizationConfig) -> QuantizedTensor:
        # 填充张量
        # ...
        
        # 调用核心量化函数
        values, scale_factors, amax = quantize_to_fp4(
            x,
            had=get_rht_matrix() if config.rht else None,
            fp4_format=config.dtype,        # nvfp4
            round_style=config.round_style,  # nearest
            scale_rule=config.scale_rule,    # static_6
            block_scale_2d=config.block_scale_2d,
            transpose=config.transpose,
        )
        
        return QuantizedTensor(values, scale_factors, amax, config.dtype, input_shape, config.scale_rule)

# pytorch/reference.py
def quantize_to_fp4(
    x: torch.Tensor,
    x_amax: torch.Tensor | None = None,
    had: torch.Tensor | None = None,
    *,
    fp4_format: DataType = DataType.nvfp4,
    round_style: RoundStyle = RoundStyle.nearest,
    scale_rule: ScaleRule = ScaleRule.mse,  # static_6
    # ...
) -> tuple[torch.Tensor, torch.Tensor, torch.Tensor | None]:
    """核心量化函数"""
    # 1. 计算全局最大值
    if x_amax is None:
        x_amax = x.abs().max().float()
    
    # 2. 分块
    x_scale_blocks = x.reshape(-1, fp4_format.block_size()).float()  # block_size=16
    
    # 3. 根据scale_rule选择量化方法
    if scale_rule in {ScaleRule.static_6, ScaleRule.static_4}:
        # 标准NVFP4量化
        x_block_scaled, scales = quantize_to_nvfp4(
            x_scale_blocks, x_amax, scale_rule=scale_rule
        )
    
    # 4. 伪量化
    x_fake_quantized = fake_quantize_to_e2m1(x_block_scaled, round_style=round_style)
    
    # 5. 打包
    x_quantized = pack_unpacked_fp4(quantize_bf16_to_unpacked_fp4(x_fake_quantized.bfloat16()))
    
    # 6. 重新排列缩放因子
    reshaped_scales = to_blocked(scales.reshape(x.shape[0], x.shape[1] // fp4_format.block_size()))
    
    return x_quantized, reshaped_scales, x_amax
```

**函数说明**:
- `quantize_to_fp4()`: 量化主函数，有三个版本（frontend、backend、reference）
- `PyTorchQuantizeBackend`: PyTorch后端实现类
- `quantize_to_nvfp4()`: NVFP4量化核心函数
- `fake_quantize_to_e2m1()`: E2M1伪量化函数

---

#### 步骤10: 运行评估任务

**文件位置**: `scripts/ptq/evaluators/evaluator.py`

```python
# 使用lm-evaluation-harness评估
from lm_eval import evaluator
from lm_eval.models.huggingface import HFLM
from lm_eval.tasks import TaskManager

full_results = evaluator.simple_evaluate(
    model=HFLM(pretrained=model, device=device, max_length=max_length),
    tasks=tasks,  # ["wikitext"]
    device=device,
    limit=limit,
    task_manager=TaskManager(include_path=(Path(__file__).parent.parent / "tasks").as_posix()),
)

# 提取结果
for task in full_results["results"]:
    result = full_results["results"][task]
    if "word_perplexity,none" in result:
        metric_name = "word_perplexity,none"
    # ...
```

**函数说明**:
- `evaluator.simple_evaluate()`: lm-eval库的评估函数
- `HFLM`: lm-eval的HuggingFace模型包装类

---

#### 步骤11: 模型推理

**文件位置**: `src/fouroversix/model/modules/linear.py` → `src/fouroversix/matmul/frontend.py`

```python
# linear.py
class FourOverSixLinearFunction(torch.autograd.Function):
    @staticmethod
    def forward(ctx, config, input, weight, bias=None):
        # 量化输入
        fprop_activation_config = config.get_activation_config()
        
        # 执行FP4矩阵乘法
        out = fp4_matmul(
            input.reshape(-1, input.shape[-1]),
            weight,  # 已量化的权重
            backend=config.matmul_backend,
            input_config=fprop_activation_config,
            out_dtype=config.output_dtype,
        ).reshape(*input.shape[:-1], weight.original_shape[0])
        
        if bias is not None:
            out = out + bias
        
        return out

# matmul/frontend.py
def fp4_matmul(
    input: torch.Tensor | QuantizedTensor,
    other: torch.Tensor | QuantizedTensor,
    *,
    backend: MatmulBackend | None = None,
    input_config: QuantizationConfig | None = None,
    other_config: QuantizationConfig | None = None,
    out_dtype: DataType = DataType.bfloat16,
) -> torch.Tensor:
    """FP4矩阵乘法主入口"""
    # 量化高精度输入
    if isinstance(input, torch.Tensor):
        input = quantize_to_fp4(input, input_config)
    
    # 选择后端
    if backend is None:
        for backend_candidate in [MatmulBackend.cutlass, MatmulBackend.pytorch]:
            if AVAILABLE_BACKENDS[backend_candidate].is_supported(input, other, out_dtype):
                backend = backend_candidate
                break
    
    return AVAILABLE_BACKENDS[backend].fp4_matmul(input, other, out_dtype=out_dtype)
```

**类/函数说明**:
- `FourOverSixLinearFunction`: 自定义autograd函数，实现量化的前向和反向传播
- `fp4_matmul()`: FP4矩阵乘法API

---

#### 步骤12: 保存结果

**文件位置**: `scripts/ptq/coordinators/base.py`

```python
def save_results(
    self,
    model_name: str,
    ptq_method: PTQMethod,
    kwargs: dict[str, Any],
    results: list[tuple[str, str, float, dict[str, Any]]],
) -> None:
    """保存结果到SQLite数据库"""
    session = self.get_session()
    
    for task, metric_name, metric_value, full_results in results:
        experiment = Experiment(
            group_name=self.group_name,
            model_name=model_name,
            task=task,
            metric_name=metric_name,
            metric_value=metric_value,
            ptq_method=ptq_method.value,
            activation_scale_rule=kwargs.get("activation_scale_rule"),
            weight_scale_rule=kwargs.get("weight_scale_rule"),
            results=full_results,
        )
        session.add(experiment)
    
    session.commit()
```

**函数说明**:
- `save_results()`: 保存评估结果到数据库
- `Experiment`: SQLAlchemy模型类，定义实验结果的数据结构

---

### 1.7.4 关键类和函数汇总表

| 类/函数名 | 文件位置 | 功能描述 |
|-----------|----------|----------|
| `cli()` | `scripts/ptq/__main__.py` | 命令行入口，解析参数 |
| `LocalEvaluationCoordinator` | `scripts/ptq/coordinators/local.py` | 本地测试协调器 |
| `BaseEvaluationCoordinator` | `scripts/ptq/coordinators/base.py` | 协调器基类 |
| `get_evaluator()` | `scripts/ptq/evaluators/__init__.py` | 评估器工厂函数 |
| `RTNEvaluator` | `scripts/ptq/evaluators/rtn.py` | RTN量化评估器 |
| `PTQEvaluator` | `scripts/ptq/evaluators/evaluator.py` | 评估器基类 |
| `quantize_model()` | `src/fouroversix/model/quantize.py` | 模型量化主函数 |
| `QuantizedModule` | `src/fouroversix/model/quantize.py` | 量化模块注册管理类 |
| `FourOverSixLinear` | `src/fouroversix/model/modules/linear.py` | 量化线性层 |
| `ModelQuantizationConfig` | `src/fouroversix/model/config.py` | 模型量化配置 |
| `quantize_to_fp4()` | `src/fouroversix/quantize/frontend.py` | 量化API入口 |
| `QuantizationConfig` | `src/fouroversix/quantize/config.py` | 张量量化配置 |
| `PyTorchQuantizeBackend` | `src/fouroversix/quantize/pytorch/backend.py` | PyTorch量化后端 |
| `quantize_to_nvfp4()` | `src/fouroversix/quantize/pytorch/reference.py` | NVFP4量化核心函数 |
| `fake_quantize_to_e2m1()` | `src/fouroversix/quantize/pytorch/reference.py` | E2M1伪量化函数 |
| `QuantizedTensor` | `src/fouroversix/quantize/quantized_tensor.py` | 量化张量类 |
| `fp4_matmul()` | `src/fouroversix/matmul/frontend.py` | FP4矩阵乘法API |
| `PyTorchMatmulBackend` | `src/fouroversix/matmul/pytorch.py` | PyTorch矩阵乘法后端 |
| `DataType` | `src/fouroversix/utils.py` | 数据类型枚举 |
| `ScaleRule` | `src/fouroversix/utils.py` | 缩放规则枚举 |
| `PTQMethod` | `scripts/ptq/utils.py` | PTQ方法枚举 |

---

## 2. 量化过程分析

### 2.1 标准NVFP4量化算法

#### 2.1.1 算法原理

标准NVFP4量化使用固定的块缩放策略，每个16元素的块使用相同的缩放因子。

**量化公式**：
```
scale = max(|x_block|) / 6
x_scaled = x / scale
x_quantized = round_to_e2m1(x_scaled)
```

**E2M1格式**：
- 1位符号位
- 2位指数位
- 1位尾数位
- 可表示值: 0, 0.5, 1, 1.5, 2, 3, 4, 6

#### 2.1.2 量化流程

```
输入张量 X [M, N]
    ↓
1. 计算全局最大值 amax = max(|X|)
    ↓
2. 分块 reshape为 [M, N/16, 16]
    ↓
3. 计算每块缩放因子
   scale = max(|x_block|) / 6 * (6 * 448 / amax)
    ↓
4. 缩放每个块
   x_scaled = x_block / (decode_scale * scale)
    ↓
5. 舍入到E2M1格式
   x_e2m1 = round_to_e2m1(x_scaled)
    ↓
6. 打包为uint8 (每字节存储2个E2M1值)
    ↓
输出: values (uint8), scales (float8_e4m3), amax (float32)
```

#### 2.1.3 PyTorch实现代码

**文件位置**: `src/fouroversix/quantize/pytorch/reference.py`

```python
def quantize_to_nvfp4(
    x_scale_blocks: torch.Tensor,  # [num_blocks, 16]
    x_amax: torch.Tensor,          # 全局最大值
    *,
    scale_rule: ScaleRule,
    scale_expansion_factor: float | None = None,
) -> tuple[torch.Tensor, torch.Tensor]:
    if x_amax == 0:
        x_scales_hp = torch.zeros(...)
    else:
        # 计算编码缩放因子
        encode_scale = (
            scale_rule.max_allowed_e2m1_value()  # 6 或 4
            * scale_rule.max_allowed_e4m3_value()  # 448 或 256
            / x_amax
        )
        # 计算每块的缩放因子
        x_scales_hp = (
            x_scale_blocks.abs().max(axis=-1).values
            / scale_rule.max_allowed_e2m1_value()
            * encode_scale
        )

    if scale_expansion_factor is not None:
        x_scales_hp = x_scales_hp * scale_expansion_factor

    # 转换为E4M3格式
    x_scales = x_scales_hp.to(torch.float8_e4m3fn)

    # 计算解码缩放因子
    decode_scale = 1 / (
        scale_rule.max_allowed_e2m1_value() 
        * scale_rule.max_allowed_e4m3_value()
        / x_amax
    )
    
    # 缩放并量化
    x_block_scaled = torch.where(
        x_scales.unsqueeze(1) != 0,
        x_scale_blocks * (1 / (decode_scale * x_scales.to(x_amax.dtype).unsqueeze(1))),
        0,
    )

    return x_block_scaled, x_scales
```

### 2.2 自适应4/6量化算法 (FourOverSix核心)

#### 2.2.1 算法原理

FourOverSix的核心创新是**自适应块缩放**，为每个块动态选择最大量化值为4或6，以最小化量化误差。

**核心思想**：
- 标准NVFP4使用最大值6，但某些块使用4可能更准确
- 通过比较两种方案的量化误差，选择更优的方案
- 支持3种误差度量：MSE、MAE、最大绝对误差

#### 2.2.2 算法流程

```
输入张量 X [M, N]
    ↓
1. 计算全局最大值 amax = max(|X|)
    ↓
2. 分块 reshape为 [M, N/16, 16]
    ↓
3. 并行计算两种量化方案：
   ┌─────────────────────────────────────┐
   │ 方案A (max=6):                      │
   │   scale_6 = max(|x_block|) / 6      │
   │   x_scaled_6 = x_block / scale_6    │
   │   x_q_6 = round_to_e2m1(x_scaled_6) │
   │   x_dq_6 = x_q_6 * scale_6          │
   │   error_6 = metric(x_block, x_dq_6) │
   └─────────────────────────────────────┘
   ┌─────────────────────────────────────┐
   │ 方案B (max=4):                      │
   │   scale_4 = max(|x_block|) / 4 * 1.5│
   │   x_scaled_4 = x_block / scale_4    │
   │   x_q_4 = round_to_e2m1(x_scaled_4) │
   │   x_dq_4 = x_q_4 * scale_4          │
   │   error_4 = metric(x_block, x_dq_4) │
   └─────────────────────────────────────┘
    ↓
4. 选择误差更小的方案
   select_4 = (error_4 < error_6)
   x_final = where(select_4, x_q_4, x_q_6)
   scale_final = where(select_4, scale_4, scale_6)
    ↓
5. 打包输出
    ↓
输出: values, scales, amax
```

#### 2.2.3 为什么使用1.5倍缩放因子

在方案B中，缩放因子乘以1.5的原因：

```
标准方案: scale_6 = max(|x_block|) / 6
方案B:    scale_4 = max(|x_block|) / 4 * 1.5 = max(|x_block|) / 6 * 1.5

这样设计的原因：
- E2M1格式的最大值为6
- 如果直接用 max/4，则量化后的最大值会被限制在4
- 乘以1.5后，相当于将量化范围从[0,4]扩展到[0,6]
- 但由于E2M1的特性，中间值(4.5, 5, 5.5等)无法精确表示
- 因此实际可表示的最大值仍然是4或6
```

#### 2.2.4 PyTorch实现代码

**文件位置**: `src/fouroversix/quantize/pytorch/reference.py`

```python
def select_fouroversix(
    x_scale_blocks: torch.Tensor,      # [num_blocks, 16] 原始数据块
    x_block_scaled_6: torch.Tensor,    # 方案A的缩放后数据
    scales_6: torch.Tensor,            # 方案A的缩放因子
    x_block_scaled_4: torch.Tensor,    # 方案B的缩放后数据
    scales_4: torch.Tensor,            # 方案B的缩放因子
    x_amax: torch.Tensor,              # 全局最大值
    *,
    scale_rule: ScaleRule = ScaleRule.mse,
    round_style: RoundStyle = RoundStyle.nearest,
) -> tuple[torch.Tensor, torch.Tensor]:
    # 1. 对两种方案进行伪量化
    x_fake_quantized_6 = fake_quantize_to_e2m1(
        x_block_scaled_6, round_style=round_style
    )
    x_fake_quantized_4 = fake_quantize_to_e2m1(
        x_block_scaled_4, round_style=round_style
    )

    # 2. 反量化以计算误差
    x_dequantized_6 = (
        x_fake_quantized_6.to(x_amax.dtype)
        * scales_6.unsqueeze(1).to(x_amax.dtype)
        * x_amax
        / (E2M1_MAX_VALUE * E4M3_MAX_FOUROVERSIX)  # 6 * 256
    )
    x_dequantized_4 = (
        x_fake_quantized_4.to(x_amax.dtype)
        * scales_4.unsqueeze(1).to(x_amax.dtype)
        * x_amax
        / (E2M1_MAX_VALUE * E4M3_MAX_FOUROVERSIX)  # 6 * 256
    )

    # 3. 计算量化误差
    if scale_rule == ScaleRule.abs_max:
        x_error_4 = (x_dequantized_4 - x_scale_blocks).abs().max(axis=-1).values
        x_error_6 = (x_dequantized_6 - x_scale_blocks).abs().max(axis=-1).values
    elif scale_rule == ScaleRule.mae:
        x_error_4 = (x_dequantized_4 - x_scale_blocks).abs().sum(axis=-1)
        x_error_6 = (x_dequantized_6 - x_scale_blocks).abs().sum(axis=-1)
    elif scale_rule == ScaleRule.mse:
        x_error_4 = ((x_dequantized_4 - x_scale_blocks) ** 2).sum(axis=-1)
        x_error_6 = ((x_dequantized_6 - x_scale_blocks) ** 2).sum(axis=-1)

    # 4. 选择误差更小的方案
    select_4 = (x_error_4 < x_error_6).unsqueeze(1)
    x_fake_quantized = torch.where(
        select_4,
        x_fake_quantized_4.reshape(x_scale_blocks.shape[0], -1),
        x_fake_quantized_6.reshape(x_scale_blocks.shape[0], -1),
    )
    scales = torch.where(
        select_4,
        scales_4.reshape(-1, 1),
        scales_6.reshape(-1, 1),
    )

    return x_fake_quantized, scales
```

#### 2.2.5 E2M1伪量化函数

**文件位置**: `src/fouroversix/quantize/pytorch/reference.py`

```python
def fake_quantize_to_e2m1(
    x: torch.Tensor,
    *,
    round_style: RoundStyle = RoundStyle.nearest,
) -> torch.Tensor:
    """
    将浮点数量化为E2M1格式的伪量化函数
    
    E2M1可表示的值:
    - |x| < 2:  0, 0.5, 1, 1.5
    - |x| < 4:  2, 3
    - |x| >= 4: 4, 6
    """
    if round_style == RoundStyle.nearest:
        # 最近邻舍入
        step1 = torch.round(2 * x.abs()) / 2  # 用于 |x| < 2
        step2 = torch.round(x.abs())          # 用于 2 <= |x| < 4
        step3 = 2 * torch.round(x.abs() / 2)  # 用于 |x| >= 4
    elif round_style == RoundStyle.stochastic:
        # 随机舍入
        rbits = torch.rand_like(x.abs()) - 0.5
        step1 = torch.round(2 * x.abs() + rbits) / 2
        step2 = torch.round(x.abs() + rbits)
        step3 = 2 * torch.round(x.abs() / 2 + rbits)
        step3[step3 > E2M1_MAX_VALUE] = E2M1_MAX_VALUE

    # 根据数值范围选择合适的量化步长
    mask1 = x.abs() < 2
    mask2 = x.abs() < 4

    return x.sign() * (
        step1 * mask1 
        + step2 * (~mask1) * mask2 
        + step3 * (~mask1) * (~mask2)
    )
```

### 2.3 量化流程总结

#### 2.3.1 完整量化函数

**文件位置**: `src/fouroversix/quantize/pytorch/reference.py`

```python
def quantize_to_fp4(
    x: torch.Tensor,
    x_amax: torch.Tensor | None = None,
    had: torch.Tensor | None = None,
    *,
    block_scale_2d: bool = False,
    fp4_format: DataType = DataType.nvfp4,
    round_style: RoundStyle = RoundStyle.nearest,
    scale_rule: ScaleRule = ScaleRule.mse,
    transpose: bool = False,
) -> tuple[torch.Tensor, torch.Tensor, torch.Tensor | None]:
    # 1. 可选：应用随机Hadamard变换
    if had is not None:
        x = (x.reshape(-1, had.shape[0]) @ had).reshape_as(x)

    # 2. 计算全局最大值
    if x_amax is None:
        x_amax = x.abs().max().float()

    # 3. 分块
    if block_scale_2d:
        # 2D块缩放 (16x16块)
        x_scale_blocks = x.reshape(
            -1, fp4_format.block_size(),
            x.shape[1] // fp4_format.block_size(),
            fp4_format.block_size()
        ).permute(0, 2, 1, 3).reshape(-1, fp4_format.block_size() ** 2)
    else:
        # 1D块缩放 (1x16块)
        x_scale_blocks = x.reshape(-1, fp4_format.block_size()).float()

    # 4. 根据量化格式选择量化方法
    if fp4_format == DataType.mxfp4:
        # MXFP4量化
        x_block_scaled, scales = quantize_to_mxfp4(
            x_scale_blocks, scale_rule=scale_rule
        )
    elif scale_rule in {ScaleRule.static_6, ScaleRule.static_4}:
        # 标准NVFP4量化
        x_block_scaled, scales = quantize_to_nvfp4(
            x_scale_blocks, x_amax, scale_rule=scale_rule
        )
    else:
        # FourOverSix自适应量化
        x_block_scaled_6, scales_6 = quantize_to_nvfp4(
            x_scale_blocks, x_amax, scale_rule=scale_rule
        )
        x_block_scaled_4, scales_4 = quantize_to_nvfp4(
            x_scale_blocks, x_amax,
            scale_rule=scale_rule,
            scale_expansion_factor=1.5
        )
        x_fake_quantized, scales = select_fouroversix(
            x_scale_blocks,
            x_block_scaled_6, scales_6,
            x_block_scaled_4, scales_4,
            x_amax,
            scale_rule=scale_rule,
            round_style=round_style
        )

    # 5. 伪量化（如果还没进行）
    if x_fake_quantized is None:
        x_fake_quantized = fake_quantize_to_e2m1(
            x_block_scaled, round_style=round_style
        )

    # 6. 打包为uint8
    x_quantized = pack_unpacked_fp4(
        quantize_bf16_to_unpacked_fp4(
            x_fake_quantized.bfloat16().reshape_as(x)
        )
    )

    # 7. 重新排列缩放因子
    reshaped_scales = to_blocked(
        scales.reshape(x.shape[0], x.shape[1] // fp4_format.block_size())
    )

    return x_quantized, reshaped_scales, x_amax
```

---

## 3. 反量化过程分析

### 3.1 反量化原理

反量化是量化的逆过程，将低精度FP4数据恢复为高精度浮点数。

#### 3.1.1 NVFP4反量化公式

```
x_dequantized = x_e2m1 * scale * amax / (max_e2m1 * max_e4m3)
```

其中：
- `x_e2m1`: E2M1格式的值
- `scale`: E4M3格式的块缩放因子
- `amax`: 全局最大值
- `max_e2m1`: 6 (标准) 或 4
- `max_e4m3`: 448 (标准) 或 256 (4/6)

### 3.2 反量化实现

#### 3.2.1 QuantizedTensor类

**文件位置**: `src/fouroversix/quantize/quantized_tensor.py`

```python
@dataclass
class QuantizedTensor:
    """量化张量类，存储量化后的数据和元信息"""
    values: torch.Tensor          # 打包的E2M1值 (uint8)
    scale_factors: torch.Tensor   # 缩放因子 (float8_e4m3 或 uint8)
    amax: torch.Tensor            # 全局最大值 (float32)
    dtype: DataType               # 数据类型 (nvfp4/mxfp4)
    original_shape: tuple[int, int]  # 原始形状
    scale_rule: ScaleRule         # 缩放规则
    padded_shape: tuple[int, int] # 填充后的形状
```

#### 3.2.2 反量化函数

**文件位置**: `src/fouroversix/quantize/quantized_tensor.py`

```python
def dequantize(self, dtype: torch.dtype = torch.bfloat16) -> torch.Tensor:
    """将量化张量反量化为高精度张量"""
    
    # 1. 解包E2M1值
    values = unpack_packed_fp4(self.values).to(dtype)
    
    # 2. 恢复缩放因子布局
    scales = from_blocked(
        self.scale_factors,
        (
            self.padded_shape[0],
            self.padded_shape[1] // self.dtype.block_size(),
        ),
    )

    # 3. 计算反量化结果
    result = values * scales.to(dtype).repeat_interleave(
        self.dtype.block_size(), -1
    )

    # 4. 对于NVFP4，需要额外处理amax
    if self.dtype == DataType.nvfp4 and self.amax is not None:
        result = (
            result.to(torch.float32)
            * self.amax
            / (
                self.scale_rule.max_allowed_e2m1_value()  # 6 或 4
                * self.scale_rule.max_allowed_e4m3_value()  # 448 或 256
            )
        ).to(dtype)

    # 5. 裁剪到原始形状
    if result.shape != self.original_shape:
        result = result[:self.original_shape[0], :self.original_shape[1]]

    return result
```

#### 3.2.3 E2M1解包函数

**文件位置**: `src/fouroversix/quantize/quantized_tensor.py`

```python
def unpack_packed_fp4(
    x: torch.Tensor,  # uint8 packed values
    to_dtype: torch.dtype = torch.float8_e4m3fn,
) -> torch.Tensor:
    """解包uint8格式的E2M1值"""
    
    # 提取低4位和高4位
    high = (x >> 4) & 0xF
    low = x & 0xF

    # 转换为FP8格式以便后续计算
    return torch.stack(
        [convert_e2m1_to_fp8_e4m3(low), convert_e2m1_to_fp8_e4m3(high)],
        dim=-1,
    ).reshape(x.shape[0], x.shape[1] * 2)


def convert_e2m1_to_fp8_e4m3(x: torch.Tensor) -> torch.Tensor:
    """将E2M1格式转换为FP8 E4M3格式"""
    sign = (x >> 3) & 0x1
    exponent = (x >> 1) & 0x3
    mantissa = x & 0x1

    # 调整指数和尾数
    new_exponent = torch.where(
        (exponent == 0) & (mantissa == 0),
        0,
        (exponent + 6) & 0xF,
    )
    new_mantissa = torch.where(exponent == 0, 0, mantissa << 2)

    return ((sign << 7) | (new_exponent << 3) | new_mantissa).view(torch.float8_e4m3fn)
```

### 3.3 缩放因子布局转换

#### 3.3.1 to_blocked函数

**文件位置**: `src/fouroversix/quantize/utils.py`

```python
def to_blocked(a: torch.Tensor) -> torch.Tensor:
    """
    将缩放因子转换为Blackwell GPU要求的blocked布局
    
    输入: [M, N/block_size]
    输出: [M*N/block_size] (blocked layout)
    """
    return (
        a.view(a.shape[0] // 128, 128, a.shape[1] // 4, 4)
        .transpose(1, 2)
        .reshape(-1, 4, 32, 4)
        .transpose(1, 2)
        .reshape(-1, 32, 16)
        .flatten()
    )
```

#### 3.3.2 from_blocked函数

**文件位置**: `src/fouroversix/quantize/quantized_tensor.py`

```python
def from_blocked(a: torch.Tensor, orig_shape: tuple[int, int]) -> torch.Tensor:
    """
    将blocked布局的缩放因子恢复为正常布局
    
    输入: [M*N/block_size] (blocked layout)
    输出: [M, N/block_size]
    """
    rows, cols = orig_shape
    return (
        a.view(-1, 32, 4, 4)
        .transpose(1, 2)
        .reshape(-1, cols // 4, 128, 4)
        .transpose(1, 2)
        .reshape(rows, cols)
    )
```

### 3.4 两种量化算法的反量化对比

| 特性 | 标准NVFP4 | FourOverSix 4/6 |
|------|-----------|-----------------|
| 缩放因子 | E4M3，最大值448 | E4M3，最大值256 |
| E2M1最大值 | 固定为6 | 每块可选4或6 |
| 反量化公式 | `x * scale * amax / (6 * 448)` | `x * scale * amax / (6 * 256)` |
| 精度 | 较低 | 更高（自适应选择） |

---

## 4. 核心实现详解

### 4.1 PyTorch后端实现（重点）

#### 4.1.1 后端类结构

**文件位置**: `src/fouroversix/quantize/pytorch/backend.py`

```python
class PyTorchQuantizeBackend(QuantizeBackendBase):
    """
    PyTorch量化后端
    - 支持所有量化选项
    - 可在非Blackwell GPU上运行
    - 速度较慢，主要用作参考实现
    """

    @classmethod
    def is_available(cls) -> bool:
        return True  # 始终可用

    @classmethod
    def is_supported(cls, x: torch.Tensor, config: QuantizationConfig) -> bool:
        return True  # 支持所有配置

    @classmethod
    def quantize_to_fp4(
        cls, x: torch.Tensor, config: QuantizationConfig
    ) -> QuantizedTensor:
        # 1. 填充张量以满足硬件要求
        # 2. 调用核心量化函数
        # 3. 返回QuantizedTensor对象
```

#### 4.1.2 核心量化函数详解

**BF16到未打包FP4的转换**：

**文件位置**: `src/fouroversix/quantize/pytorch/reference.py`

```python
def quantize_bf16_to_unpacked_fp4(x: torch.Tensor) -> torch.Tensor:
    """
    将BF16张量直接转换为未打包的FP4编码
    
    这是通过位操作实现的快速转换，避免了浮点运算
    """
    assert x.dtype == torch.bfloat16

    bx = x.view(torch.int16)
    s = (bx >> 15) & 0x1      # 符号位 (1位)
    e = (bx >> 7) & 0xFF      # 指数位 (8位)
    m = bx & 0x7F             # 尾数位 (7位)
    is_zero = (e == 0) & (m == 0)

    # 提取尾数的最高位
    m = (m >> 6) & 1
    
    # 处理0.5的特殊情况
    is_half = (e == 126) & (m == 0)
    m = torch.where(is_half, torch.tensor(1, dtype=torch.int16, device=x.device), m)

    # 指数映射
    # BF16: exp=126 -> E2M1: E=0 (次正规数)
    # BF16: exp=127 -> E2M1: E=1
    # BF16: exp=128 -> E2M1: E=2
    # BF16: exp=129 -> E2M1: E=3
    e = e - 126
    e = torch.where(is_zero, torch.tensor(0, dtype=torch.int16, device=x.device), e)

    # 零值处理
    m = torch.where(is_zero, torch.tensor(0, dtype=torch.int16, device=x.device), m)

    # 组合成E2M1编码: [S|E[1:0]|M]
    code = (s << 3) | (e << 1) | m
    return code.to(torch.uint8)
```

**FP4打包函数**：

**文件位置**: `src/fouroversix/quantize/pytorch/reference.py`

```python
def pack_unpacked_fp4(x: torch.Tensor) -> torch.Tensor:
    """
    将未打包的FP4值打包为uint8
    每个uint8存储2个FP4值（高4位和低4位）
    """
    assert x.dtype == torch.uint8

    dim = 1
    size_along_dim = x.size(dim)
    new_size_along_dim = (size_along_dim + 1) // 2

    # 处理奇数长度
    if size_along_dim % 2 != 0:
        pad_sizes = [0] * (2 * x.ndim)
        pad_index = (x.ndim - dim - 1) * 2 + 1
        pad_sizes[pad_index] = 1
        x = torch.nn.functional.pad(x, pad_sizes, mode="constant", value=0)

    # 重塑并打包
    new_shape = list(x.shape)
    new_shape[dim] = new_size_along_dim
    new_shape.insert(dim + 1, 2)
    x = x.reshape(*new_shape)

    low = x.select(dim + 1, 0)
    high = x.select(dim + 1, 1)
    return (high << 4) | low
```

#### 4.1.3 MXFP4量化实现

**文件位置**: `src/fouroversix/quantize/pytorch/reference.py`

```python
def quantize_to_mxfp4(
    x_scale_blocks: torch.Tensor,
    *,
    scale_rule: ScaleRule = ScaleRule.mse,
) -> tuple[torch.Tensor, torch.Tensor]:
    """
    MXFP4量化
    
    与NVFP4的区别：
    1. 块大小为32（NVFP4为16）
    2. 缩放因子为E8M0格式（8位指数，无尾数）
    3. 不支持自适应缩放
    """
    assert scale_rule in {ScaleRule.static_6, ScaleRule.static_4}

    # 计算缩放因子（E8M0格式）
    x_scales_hp = (
        x_scale_blocks.abs().max(axis=-1).values 
        / scale_rule.max_allowed_e2m1_value()
    )
    x_scales_e8m0_u32 = x_scales_hp.view(torch.int32)

    # 提取8位指数作为缩放因子
    x_scales_e8m0 = ((x_scales_e8m0_u32 >> 23) & 0xFF).to(torch.uint8)

    # 向上取整
    x_scales = torch.where(
        (x_scales_e8m0_u32 & 0x7FFFFF) == 0,
        x_scales_e8m0,
        x_scales_e8m0 + 1,
    )

    # 转换回浮点数
    x_scales_hp = (x_scales.to(torch.int32) << 23).view(torch.float32)

    # 缩放数据块
    x_block_scaled = x_scale_blocks / x_scales_hp.unsqueeze(1)

    return x_block_scaled, x_scales.view(torch.float8_e8m0fnu)
```

### 4.2 Triton后端实现（简介）

**文件位置**: `src/fouroversix/quantize/triton/kernel.py`

#### 4.2.1 特点

- 支持所有量化选项（随机舍入、RHT、2D块缩放等）
- 需要Blackwell GPU
- 速度介于PyTorch和CUDA之间
- 支持训练

#### 4.2.2 核心Kernel

Triton实现包含两个主要kernel：

1. **block_scaled_fp4_quantization_kernel**: 标准NVFP4/MXFP4量化
2. **nvfp4_fouroversix_quantization_kernel**: FourOverSix自适应量化

关键特性：
- 使用TensorDescriptor进行高效内存访问
- 内联PTX汇编实现E2M1转换
- 支持随机舍入（通过`cvt.rs.satfinite.e2m1x4.f32`指令）

### 4.3 CUDA后端实现（简介）

**文件位置**: `src/fouroversix/csrc/include/fp4_quant.h`

#### 4.3.1 特点

- 最快的实现
- 仅支持推理（不支持随机舍入、RHT、转置等）
- 需要Blackwell GPU
- 使用CUTLASS进行矩阵乘法

#### 4.3.2 参数结构

```cpp
struct FP4_quant_params {
    void *__restrict__ x_ptr;          // 输入数据
    void *__restrict__ x_e2m1_ptr;     // 输出E2M1值
    void *__restrict__ x_sf_ptr;       // 输出缩放因子
    void *__restrict__ amax_ptr;       // 全局最大值
    
    int M, N, M_rounded, N_rounded;    // 维度信息
    bool is_nvfp4;                     // 是否为NVFP4
    bool is_rtn;                       // 是否最近邻舍入
    bool is_rht;                       // 是否使用RHT
    bool is_4o6;                       // 是否使用4/6
    int selection_rule;                // 选择规则
};
```

### 4.4 后端选择策略

**文件位置**: `src/fouroversix/quantize/frontend.py`

```python
def quantize_to_fp4(x: torch.Tensor, config: QuantizationConfig) -> QuantizedTensor:
    if config.backend is None:
        # 自动选择后端
        for backend in [QuantizeBackend.cuda, QuantizeBackend.triton, QuantizeBackend.pytorch]:
            if AVAILABLE_BACKENDS[backend].is_supported(x, config):
                selected_backend = backend
                break
    else:
        selected_backend = config.backend

    return AVAILABLE_BACKENDS[selected_backend].quantize_to_fp4(x, config)
```

**选择规则**：
1. 如果没有Blackwell GPU → PyTorch
2. 如果需要随机舍入/RHT/转置/2D块缩放 → Triton
3. 否则 → CUDA（最快）

### 4.5 矩阵乘法实现

#### 4.5.1 前端API

**文件位置**: `src/fouroversix/matmul/frontend.py`

```python
def fp4_matmul(
    input: torch.Tensor | QuantizedTensor,
    other: torch.Tensor | QuantizedTensor,
    *,
    backend: MatmulBackend | None = None,
    input_config: QuantizationConfig | None = None,
    other_config: QuantizationConfig | None = None,
    out_dtype: DataType = DataType.bfloat16,
) -> torch.Tensor:
    """
    执行FP4矩阵乘法
    
    支持以下组合：
    - 高精度 × 高精度：先量化再计算
    - 高精度 × 低精度：只量化输入
    - 低精度 × 低精度：直接计算
    """
    # 自动量化高精度输入
    if isinstance(input, torch.Tensor):
        input = quantize_to_fp4(input, input_config)
    if isinstance(other, torch.Tensor):
        other = quantize_to_fp4(other, other_config)

    # 选择后端
    if backend is None:
        for backend_candidate in [MatmulBackend.cutlass, MatmulBackend.pytorch]:
            if AVAILABLE_BACKENDS[backend_candidate].is_supported(input, other, out_dtype):
                backend = backend_candidate
                break

    return AVAILABLE_BACKENDS[backend].fp4_matmul(input, other, out_dtype)
```

#### 4.5.2 PyTorch矩阵乘法后端

```python
class PyTorchMatmulBackend:
    @classmethod
    def fp4_matmul(
        cls,
        input: QuantizedTensor,
        other: QuantizedTensor,
        out_dtype: DataType = DataType.bfloat16,
    ) -> torch.Tensor:
        # 反量化
        input_dequantized = input.dequantize(out_dtype.torch_dtype())
        other_dequantized = other.dequantize(out_dtype.torch_dtype())

        # 执行矩阵乘法
        return torch.matmul(input_dequantized, other_dequantized.T)
```

---

## 附录

### A. 关键常量定义

**文件位置**: `src/fouroversix/quantize/pytorch/reference.py`

```python
E2M1_MAX_VALUE = 6          # E2M1最大值
E2M1_MAX_FOUR = 4           # E2M1最大值（4/6方案B）
E4M3_MAX_VALUE = 448        # E4M3最大值（标准）
E4M3_MAX_FOUROVERSIX = 256  # E4M3最大值（4/6）
```

### B. 数据类型映射

**文件位置**: `src/fouroversix/utils.py`

```python
class DataType(str, Enum):
    mxfp4 = "mxfp4"  # 块大小32，缩放因子E8M0
    nvfp4 = "nvfp4"  # 块大小16，缩放因子E4M3

    def block_size(self) -> int:
        return {DataType.mxfp4: 32, DataType.nvfp4: 16}[self]

    def scale_dtype(self) -> torch.dtype:
        return {
            DataType.mxfp4: torch.float8_e8m0fnu,
            DataType.nvfp4: torch.float8_e4m3fn,
        }[self]
```

### C. 参考文献

1. Four Over Six论文: https://arxiv.org/abs/2512.02010
2. NVIDIA Blackwell架构文档: https://docs.nvidia.com/cutlass/latest/media/docs/cpp/blackwell_functionality.html
3. MXFP规范: https://www.opencompute.org/documents/ocp-microscaling-formats-mx-v1-0-spec-final-pdf

---

## 5. MXFP4格式FourOverSix算法修改记录

### 5.1 修改概述

本节记录了对代码仓库的修改，以支持MXFP4格式的FourOverSix自适应量化算法。

### 5.2 修改文件列表

| 文件路径 | 修改类型 | 修改说明 |
|----------|----------|----------|
| `src/fouroversix/utils.py` | 修改 | 新增`is_adaptive()`方法，更新文档注释 |
| `src/fouroversix/quantize/backend.py` | 修改 | 移除MXFP4只支持静态规则的限制 |
| `src/fouroversix/quantize/pytorch/reference.py` | 修改 | 新增`select_fouroversix_mxfp4()`函数，重构量化分支逻辑 |
| `tests/test_correctness.py` | 新增 | 新增MXFP4量化测试用例 |

### 5.3 详细修改说明

#### 5.3.1 `src/fouroversix/utils.py`

**修改内容**：
1. 更新`ScaleRule`类的文档注释，说明支持NVFP4和MXFP4
2. 新增`is_adaptive()`方法，用于判断是否为自适应缩放规则

```python
def is_adaptive(self) -> bool:
    """Return True if the rule is adaptive (4/6 selection), False otherwise.
    
    [MODIFIED] 新增方法：判断是否为自适应缩放规则
    自适应规则包括: mse, mae, abs_max
    静态规则包括: static_6, static_4
    """
    return self in {ScaleRule.mse, ScaleRule.mae, ScaleRule.abs_max}
```

#### 5.3.2 `src/fouroversix/quantize/backend.py`

**修改内容**：
移除MXFP4只支持静态规则的限制，允许MXFP4使用自适应缩放规则

```python
# [MODIFIED] 移除MXFP4只支持静态规则的限制
# MXFP4现在支持自适应缩放规则(mse, mae, abs_max)以实现FourOverSix算法
# 原代码:
# if config.dtype == DataType.mxfp4 and config.scale_rule not in {
#     ScaleRule.static_6,
#     ScaleRule.static_4,
# }:
#     msg = (
#         "MXFP4 quantization only supports the `static_6` and `static_4` scale "
#         "rules"
#     )
#     raise ValueError(msg)
```

#### 5.3.3 `src/fouroversix/quantize/pytorch/reference.py`

**修改内容**：

1. **修改`quantize_to_mxfp4()`函数**：移除断言，允许自适应规则调用

```python
def quantize_to_mxfp4(
    x_scale_blocks: torch.Tensor,
    *,
    scale_rule: ScaleRule = ScaleRule.mse,
) -> tuple[torch.Tensor, torch.Tensor]:
    """
    [MODIFIED] 修改MXFP4量化函数，移除只支持静态规则的断言
    现在支持自适应缩放规则，但此函数仅用于静态规则
    """
    # [MODIFIED] 移除断言，允许自适应规则调用此函数
    # assert scale_rule in {ScaleRule.static_6, ScaleRule.static_4}
    ...
```

2. **新增`select_fouroversix_mxfp4()`函数**：MXFP4格式的FourOverSix自适应选择函数

```python
def select_fouroversix_mxfp4(
    x_scale_blocks: torch.Tensor,
    x_block_scaled_6: torch.Tensor,
    scales_6: torch.Tensor,
    x_block_scaled_4: torch.Tensor,
    scales_4: torch.Tensor,
    *,
    scale_rule: ScaleRule = ScaleRule.mse,
    round_style: RoundStyle = RoundStyle.nearest,
) -> tuple[torch.Tensor, torch.Tensor]:
    """
    [NEW] 新增函数：MXFP4格式的FourOverSix自适应选择函数
    
    与NVFP4的select_fouroversix函数类似，但针对MXFP4的E8M0缩放因子格式进行调整
    
    Args:
        x_scale_blocks: 原始数据块 [num_blocks, 32]
        x_block_scaled_6: 方案A(max=6)的缩放后数据
        scales_6: 方案A的E8M0格式缩放因子
        x_block_scaled_4: 方案B(max=4)的缩放后数据
        scales_4: 方案B的E8M0格式缩放因子
        scale_rule: 误差度量规则 (mse/mae/abs_max)
        round_style: 舍入方式
    
    Returns:
        x_fake_quantized: 选择的伪量化结果
        scales: 选择的缩放因子 (E8M0格式)
    """
    # 1. 对两种方案进行伪量化
    # 2. 反量化以计算误差（MXFP4使用E8M0格式）
    # 3. 计算量化误差
    # 4. 选择误差更小的方案
    ...
```

3. **重构`quantize_to_fp4()`函数**：添加MXFP4自适应量化分支

```python
# [MODIFIED] 重构量化分支逻辑，支持MXFP4的自适应量化
if fp4_format == DataType.mxfp4:
    # MXFP4量化分支
    if scale_rule.is_adaptive():
        # [NEW] MXFP4自适应量化 (FourOverSix for MXFP4)
        # 方案A: max=6
        x_block_scaled_6, scales_6 = quantize_to_mxfp4(
            x_scale_blocks,
            scale_rule=ScaleRule.static_6,
        )
        # 方案B: max=4
        x_block_scaled_4, scales_4 = quantize_to_mxfp4(
            x_scale_blocks,
            scale_rule=ScaleRule.static_4,
        )
        # 调用MXFP4的自适应选择函数
        x_fake_quantized, scales = select_fouroversix_mxfp4(
            x_scale_blocks,
            x_block_scaled_6,
            scales_6,
            x_block_scaled_4,
            scales_4,
            scale_rule=scale_rule,
            round_style=round_style,
        )
    else:
        # MXFP4静态量化 (static_6 或 static_4)
        x_block_scaled, scales = quantize_to_mxfp4(
            x_scale_blocks,
            scale_rule=scale_rule,
        )
```

#### 5.3.4 `tests/test_correctness.py`

**新增测试**：

1. **`test_mxfp4_quantization()`**: 测试MXFP4格式的量化，包括自适应缩放规则

```python
@pytest.mark.parametrize("input_type", ["zeros", "ones", "rand01", "randn"])
@pytest.mark.parametrize("input_shape", [(1024, 1024), (1024, 512), (512, 1024)])
@pytest.mark.parametrize("scale_rule", [
    ScaleRule.abs_max,
    ScaleRule.mae,
    ScaleRule.mse,
    ScaleRule.static_4,
    ScaleRule.static_6,
])
def test_mxfp4_quantization(input_type, input_shape, scale_rule):
    """
    [NEW] 测试MXFP4格式的量化，包括自适应缩放规则
    
    测试内容：
    1. 静态规则 (static_6, static_4) - 标准MXFP4量化
    2. 自适应规则 (mse, mae, abs_max) - MXFP4 FourOverSix量化
    """
    ...
```

2. **`test_mxfp4_vs_nvfp4_quantization()`**: 对比MXFP4和NVFP4的量化结果

```python
@pytest.mark.parametrize("input_shape", [(256, 256)])
@pytest.mark.parametrize("scale_rule", [ScaleRule.mse, ScaleRule.static_6])
def test_mxfp4_vs_nvfp4_quantization(input_shape, scale_rule):
    """
    [NEW] 对比MXFP4和NVFP4的量化结果
    
    验证：
    1. 两种格式的量化都能正常工作
    2. 自适应规则在两种格式下都能正常工作
    """
    ...
```

### 5.4 MXFP4与NVFP4 FourOverSix对比

| 特性 | NVFP4 FourOverSix | MXFP4 FourOverSix |
|------|-------------------|-------------------|
| 块大小 | 16 | 32 |
| 缩放因子格式 | E4M3 (float8) | E8M0 (uint8) |
| 全局amax | 需要 | 不需要 |
| 方案A缩放 | `max/6` | `max/6` (E8M0) |
| 方案B缩放 | `max/6 * 1.5` | `max/4` (E8M0) |
| 反量化公式 | `x * scale * amax / (6 * 256)` | `x * scale_e8m0` |
| 自适应选择函数 | `select_fouroversix()` | `select_fouroversix_mxfp4()` |

### 5.5 使用示例

#### 5.5.1 MXFP4标准量化

```python
from fouroversix import quantize_to_fp4, QuantizationConfig, DataType, ScaleRule

config = QuantizationConfig(
    dtype=DataType.mxfp4,
    scale_rule=ScaleRule.static_6,  # 或 ScaleRule.static_4
)

quantized = quantize_to_fp4(x, config)
```

#### 5.5.2 MXFP4 FourOverSix自适应量化

```python
from fouroversix import quantize_to_fp4, QuantizationConfig, DataType, ScaleRule

config = QuantizationConfig(
    dtype=DataType.mxfp4,
    scale_rule=ScaleRule.mse,  # 或 ScaleRule.mae, ScaleRule.abs_max
)

quantized = quantize_to_fp4(x, config)
```

#### 5.5.3 模型量化

```python
from fouroversix import ModelQuantizationConfig, DataType, ScaleRule
from fouroversix.model import quantize_model

config = ModelQuantizationConfig(
    dtype=DataType.mxfp4,
    activation_scale_rule=ScaleRule.mse,
    weight_scale_rule=ScaleRule.mse,
)

quantize_model(model, config)
```

### 5.6 注意事项

1. **E8M0缩放因子格式**：MXFP4使用E8M0格式（8位指数，无尾数），与NVFP4的E4M3格式不同
2. **块大小差异**：MXFP4块大小为32，NVFP4为16，这会影响量化粒度
3. **无全局amax**：MXFP4不需要全局amax，每个块的缩放因子独立计算
4. **后端兼容性**：目前只有PyTorch后端完整支持MXFP4的自适应量化，CUDA和Triton后端需要额外修改

### 5.7 新增功能（v2）

#### 5.7.1 强制使用max=4参数

**修改文件**：`src/fouroversix/quantize/config.py`

新增 `force_max_4` 参数，用于强制FourOverSix算法使用max=4方案：

```python
@dataclass
class QuantizationConfig:
    # ...
    force_max_4: bool = False  # [NEW] 强制使用max=4
    # ...
```

**使用示例**：

```python
from fouroversix import quantize_to_fp4, QuantizationConfig, DataType, ScaleRule

# 强制使用max=4（跳过自适应选择）
config = QuantizationConfig(
    dtype=DataType.mxfp4,
    scale_rule=ScaleRule.mse,  # 自适应规则
    force_max_4=True,  # 强制使用max=4
)

quantized = quantize_to_fp4(x, config)
```

#### 5.7.2 FourOverSix选择日志

**修改文件**：`src/fouroversix/quantize/config.py`

新增 `log_fouroversix` 参数，用于记录自适应选择的统计信息：

```python
@dataclass
class QuantizationConfig:
    # ...
    log_fouroversix: bool = False  # [NEW] 记录FourOverSix选择日志
    # ...
```

**日志输出示例**：

```
============================================================
[NVFP4 FourOverSix] 自适应选择统计:
  总块数: 1024
  选择 max=4 的块数: 423 (41.31%)
  选择 max=6 的块数: 601 (58.69%)
  误差度量规则: mse
  全局amax: 12.345678
  max=4 误差统计: mean=0.001234, min=0.000001, max=0.012345
  max=6 误差统计: mean=0.002345, min=0.000002, max=0.023456
  强制使用 max=4: False
============================================================
```

**使用示例**：

```python
import logging

# 配置日志级别
logging.basicConfig(level=logging.INFO)

from fouroversix import quantize_to_fp4, QuantizationConfig, DataType, ScaleRule

config = QuantizationConfig(
    dtype=DataType.mxfp4,
    scale_rule=ScaleRule.mse,
    log_fouroversix=True,  # 启用日志
)

quantized = quantize_to_fp4(x, config)
```

#### 5.7.3 修改文件列表

| 文件路径 | 修改类型 | 修改说明 |
|----------|----------|----------|
| `src/fouroversix/quantize/config.py` | 修改 | 新增 `force_max_4` 和 `log_fouroversix` 参数 |
| `src/fouroversix/quantize/pytorch/reference.py` | 修改 | 新增日志记录功能，支持强制使用max=4 |
| `src/fouroversix/quantize/pytorch/backend.py` | 修改 | 传递新参数到量化函数 |

#### 5.7.4 日志输出字段说明

| 字段 | 说明 |
|------|------|
| 总块数 | 输入张量分块后的总块数 |
| 选择 max=4 的块数 | 自适应选择max=4方案的块数和比例 |
| 选择 max=6 的块数 | 自适应选择max=6方案的块数和比例 |
| 误差度量规则 | 使用的误差度量方法（mse/mae/abs_max） |
| 全局amax | 输入张量的全局最大值（仅NVFP4） |
| max=4 误差统计 | max=4方案的误差均值、最小值、最大值 |
| max=6 误差统计 | max=6方案的误差均值、最小值、最大值 |
| 强制使用 max=4 | 是否强制使用max=4 |

---

## 6. 数据集生成与测试

### 6.1 为什么wikitext上都是max=6

**FourOverSix算法的核心**：
- **max=6**：量化范围大（0-6），量化步长大，适合有离群值的数据
- **max=4**：量化范围小（0-4），量化步长小，适合数据集中在较小值的情况

**wikitext的特点**：
- 自然语言数据，激活值分布有长尾特性
- 存在离群值，导致块最大值较大
- 最大值/6的分布更常见

**max=4更优的条件**：
1. 块最大值接近4的倍数（4.0, 8.0, 12.0等）
2. 数据分布均匀，无大离群值
3. 数据集中在较小值范围

### 6.2 生成适合max=4的数据集

**脚本位置**：`scripts/generate_fouroversix_dataset.py`

```bash
# 生成所有类型的数据集
python scripts/generate_fouroversix_dataset.py --type all --num-samples 1000 --output-dir datasets

# 只生成文本数据集
python scripts/generate_fouroversix_dataset.py --type text --num-samples 1000

# 只生成激活值数据集
python scripts/generate_fouroversix_dataset.py --type activation --num-samples 1000

# 生成对比数据集（包含多种分布）
python scripts/generate_fouroversix_dataset.py --type comparison --num-samples 100
```

**生成的数据集**：

| 数据集类型 | 文件位置 | 说明 |
|------------|----------|------|
| 文本数据集 | `datasets/fouroversix_text/` | 兼容lm-eval框架 |
| 激活值数据集 | `datasets/fouroversix_activations/` | 直接测试量化效果 |
| 对比数据集 | `datasets/fouroversix_comparison/` | 包含多种分布类型 |

### 6.3 直接测试激活值量化

**脚本位置**：`test_activation_quantization.py`

```bash
# 基本测试
python test_activation_quantization.py --dtype mxfp4 --scale-rule mse

# 比较不同策略
python test_activation_quantization.py --dtype mxfp4 --compare

# 自定义数据形状
python test_activation_quantization.py --shape 512,512 --compare
```

**测试的数据分布类型**：

| 分布类型 | 生成函数 | 特点 |
|----------|----------|------|
| max=4优化 | `generate_max4_optimized_activation` | 块最大值接近4的倍数 |
| 均匀分布 | `generate_uniform_activation` | 数据在[-4, 4]均匀分布 |
| 正态分布 | `generate_normal_activation` | 标准正态分布×2 |
| 稀疏分布 | `generate_sparse_activation` | 90%的值为0 |
| 双峰分布 | `generate_bimodal_activation` | 峰值在2和4附近 |
| 有离群值 | `generate_outlier_activation` | 类似wikitext |
| 最大值恰好为4 | `generate_exact_max4_activation` | 最有利于max=4 |

### 6.4 使用新数据集进行测试

**方式1：使用lm-eval框架**

```bash
# 先生成数据集
python scripts/generate_fouroversix_dataset.py --type text

# 使用新数据集测试
python -m scripts.ptq \
    --model-name meta-llama/Llama-3.2-1B \
    --ptq-method rtn \
    --task fouroversix_max4 \
    --dtype mxfp4 \
    --a-scale-rule mse \
    --w-scale-rule mse
```

**方式2：直接测试激活值**

```bash
# 比较不同策略
python test_activation_quantization.py --dtype mxfp4 --compare
```

### 6.5 预期结果

| 数据分布 | 预期选择max=4的比例 | 最优策略 |
|----------|---------------------|----------|
| 最大值恰好为4 | > 80% | max=4 |
| max=4优化 | > 50% | 自适应或max=4 |
| 均匀分布 | > 30% | 自适应 |
| 稀疏分布 | > 20% | 自适应 |
| 正态分布 | < 20% | max=6 |
| 有离群值 | < 10% | max=6 |

### 6.6 数据集文件结构

```
datasets/
├── fouroversix_text/
│   ├── train.jsonl      # 训练集
│   ├── test.jsonl       # 测试集
│   └── metadata.json    # 元数据
├── fouroversix_activations/
│   ├── activations_train.npy
│   └── activations_test.npy
└── fouroversix_comparison/
    ├── max4_optimized.npy
    ├── uniform.npy
    ├── normal.npy
    ├── sparse.npy
    ├── bimodal.npy
    └── outlier.npy
```
