# Dr.Kernel 模型验证操作指南

## 目录

1. [验证脚本路径和文件名](#1-验证脚本路径和文件名)
2. [前置条件和环境配置](#2-前置条件和环境配置)
3. [模型文件预期路径及格式](#3-模型文件预期路径及格式)
4. [命令行执行示例](#4-命令行执行示例)
5. [关键指标解释及正常范围](#5-关键指标解释及正常范围)
6. [常见错误排查方法](#6-常见错误排查方法)
7. [验证结果输出格式及解读](#7-验证结果输出格式及解读)

---

## 1. 验证脚本路径和文件名

### 1.1 核心验证脚本

| 脚本类型 | 路径 | 说明 |
|---------|------|------|
| **主验证入口** | `drkernel/kernel/main_grading.py` | Python 主程序，使用 Hydra 配置管理 |
| **通用验证脚本** | `drkernel/kernel/scripts/eval/grading_common.sh` | 通用 Shell 脚本，包含共享逻辑 |
| **Dr.Kernel 14B 验证** | `drkernel/kernel/scripts/eval/drkernel-14b-maxturns3.sh` | Dr.Kernel 14B 模型 3 轮验证 |
| **Dr.Kernel 14B 多迭代** | `drkernel/kernel/scripts/eval/drkernel-14b-maxturns5-maxiter10.sh` | Dr.Kernel 14B 模型 5 轮 10 次迭代验证 |
| **Claude 4.5 Sonnet** | `drkernel/kernel/scripts/eval/claude-4.5-sonnet-level2.sh` | Claude API 模型验证示例 |
| **环境设置** | `drkernel/setup_env.sh` | 环境变量配置脚本 |

### 1.2 配置文件

| 配置文件 | 路径 | 说明 |
|---------|------|------|
| **验证配置** | `drkernel/kernel/config/kernel_grading.yaml` | Hydra 主配置文件 |
| **多轮提示配置** | `drkernel/kernel/config/prompt_config/multi_turn_kernel.yaml` | 多轮对话提示模板 |

### 1.3 奖励函数模块

| 模块 | 路径 | 说明 |
|------|------|------|
| **内核奖励计算** | `drkernel/kernel/rewards/kernel_reward.py` | 批量计算内核代码奖励 |
| **奖励客户端** | `drkernel/kernel/rewards/reward_client.py` | KernelServer 通信客户端 |
| **异步奖励管理器** | `drkernel/kernel/workers/reward_manager/kernel_async.py` | 异步奖励计算管理 |

---

## 2. 前置条件和环境配置

### 2.1 硬件要求

| 资源 | 最低要求 | 推荐配置 |
|------|---------|---------|
| **GPU** | 1x A100 40GB | 8x A100 80GB |
| **CPU** | 16 核 | 32+ 核 |
| **内存** | 64 GB | 256 GB |
| **存储** | 100 GB SSD | 500 GB NVMe SSD |

### 2.2 软件依赖

```bash
# Python 版本
Python >= 3.10

# 核心依赖
torch >= 2.1.0
vllm == 0.10.2
flash-attn == 2.8.3
ray >= 2.9.0
hydra-core >= 1.3.0

# 可选依赖
gradio >= 4.0.0  # 可视化界面
wandb >= 0.16.0  # 实验跟踪
```

### 2.3 环境配置步骤

```bash
# 1. 克隆项目
cd /path/to/workspace
git clone <repository_url>
cd KernelGYM/drkernel

# 2. 创建虚拟环境
conda create -n drkernel python=3.10 -y
conda activate drkernel

# 3. 安装依赖
pip install -r requirements.txt

# 4. 设置环境变量
source setup_env.sh

# 5. 配置 KernelServer URL（必需）
export KERNELGYM_SERVER_URL="http://your-server-ip:9744"
# 或在脚本中设置 REWARD_SERVER_URL
```

### 2.4 KernelServer 配置

验证过程需要 KernelServer 提供内核代码评估服务：

```bash
# 检查 KernelServer 健康状态
curl http://your-server-ip:9744/health

# 预期响应
# {"status": "healthy", "gpu_available": true, ...}
```

### 2.5 验证数据集

项目使用 HuggingFace 数据集：

```bash
# 默认验证数据集
EVAL_DATASET="hkust-nlp/drkernel-validation-data"

# 或使用本地 Parquet 文件
EVAL_DATASET="/path/to/your/validation_data.parquet"
```

---

## 3. 模型文件预期路径及格式

### 3.1 支持的模型格式

| 格式 | 说明 | 示例路径 |
|------|------|---------|
| **HuggingFace Hub** | 直接使用模型 ID | `hkust-nlp/drkernel-14b` |
| **本地目录** | 本地保存的模型 | `/models/drkernel-14b/` |
| **HDFS 路径** | 分布式存储路径 | `hdfs://cluster/models/drkernel-14b/` |

### 3.2 模型目录结构

```
/models/drkernel-14b/
├── config.json                 # 模型配置
├── model.safetensors           # 模型权重（或分片）
├── model.safetensors.index.json # 分片索引（如适用）
├── tokenizer.json              # 分词器配置
├── tokenizer_config.json       # 分词器设置
├── special_tokens_map.json     # 特殊 token 映射
├── generation_config.json      # 生成配置
└── chat_template.jinja         # 聊天模板（可选）
```

### 3.3 模型路径配置方式

**方式一：Shell 脚本变量**

```bash
# 在 eval 脚本中设置
MODEL_PATH="/models/drkernel-14b"
MODEL_NAME="drkernel-14b"
```

**方式二：Hydra 配置文件**

```yaml
# kernel_grading.yaml
model:
  path: /models/drkernel-14b
  
actor_rollout_ref:
  model:
    path: /models/drkernel-14b
```

**方式三：命令行参数**

```bash
python -m kernel.main_grading \
    model.path=/models/drkernel-14b \
    actor_rollout_ref.model.path=/models/drkernel-14b
```

### 3.4 SFT 训练输出模型路径

SFT 训练完成后，模型保存在以下位置：

```
# 默认输出路径
{output_dir}/epoch_{epoch_num}/
├── actor/
│   ├── model.safetensors
│   ├── config.json
│   └── ...
└── config.yaml

# 示例
/output/sft_run/epoch_3/actor/
```

---

## 4. 命令行执行示例

### 4.1 使用 Shell 脚本执行（推荐）

**基础验证命令**

```bash
cd drkernel/kernel/scripts/eval

# 执行 Dr.Kernel 14B 验证（3 轮多轮对话）
bash drkernel-14b-maxturns3.sh

# 执行带多迭代的验证
bash drkernel-14b-maxturns5-maxiter10.sh
```

**带参数覆盖执行**

```bash
# 覆盖模型路径
bash drkernel-14b-maxturns3.sh \
    --model_path /models/your-model \
    --eval_dataset /data/your-validation.parquet \
    --output_path /output/grading_results.parquet

# 调整生成参数
bash drkernel-14b-maxturns3.sh \
    --n_samples 16 \
    --temperature 0.7 \
    --batch_size 64

# 使用不同的 rollout 模式
bash drkernel-14b-maxturns3.sh \
    --rollout_mode standalone_vllm \
    --rollout_gpu_memory_util 0.8
```

### 4.2 直接使用 Python 命令

**基础命令**

```bash
cd drkernel

python -m kernel.main_grading \
    data.path=hkust-nlp/drkernel-validation-data \
    data.output_path=/output/graded_results.parquet \
    model.path=/models/drkernel-14b
```

**完整参数示例**

```bash
python -m kernel.main_grading \
    data.path=/data/validation.parquet \
    data.output_path=/output/results.parquet \
    data.raw_response_path=/output/raw_responses.jsonl \
    data.metrics_output_path=/output/metrics.json \
    data.n_samples=8 \
    data.batch_size=128 \
    data.solve_threshold=0.99 \
    data.pass_at_k=1 \
    model.path=/models/drkernel-14b \
    actor_rollout_ref.rollout.mode=async_vllm \
    actor_rollout_ref.rollout.temperature=1.0 \
    actor_rollout_ref.rollout.top_p=0.95 \
    actor_rollout_ref.rollout.gpu_memory_utilization=0.5 \
    actor_rollout_ref.rollout.tensor_model_parallel_size=1 \
    reward_model.server_url="http://your-server:9744" \
    reward_model.reward_weights.compilation=0.3 \
    reward_model.reward_weights.correctness=0.4 \
    reward_model.reward_weights.performance=0.3 \
    trainer.nnodes=1 \
    trainer.n_gpus_per_node=8 \
    trainer.project_name=my-grading \
    trainer.experiment_name=test-run
```

### 4.3 可视化模式

**生成后启动 Gradio 可视化**

```bash
# 在脚本中设置
GRADIO_VISUALIZATION=True
GRADIO_SHARE=True

# 或通过命令行
python -m kernel.main_grading \
    gradio=True \
    gradio_share=True \
    ...
```

**仅启动可视化（不重新生成）**

```bash
python -m kernel.main_grading \
    visualize_only=True \
    visualize_dir=/output/eval_outputs
```

### 4.4 使用 OpenAI API 模型

```bash
# 配置 OpenAI 后端
python -m kernel.main_grading \
    actor_rollout_ref.rollout.backend=openai \
    actor_rollout_ref.rollout.openai.model="anthropic/claude-sonnet-4.5" \
    actor_rollout_ref.rollout.openai.api_key="your-api-key" \
    actor_rollout_ref.rollout.openai.base_url="https://api.openai.com/v1" \
    actor_rollout_ref.rollout.openai.timeout=120 \
    actor_rollout_ref.rollout.openai.max_retries=3 \
    actor_rollout_ref.rollout.openai.max_concurrency=30
```

### 4.5 关键参数说明

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `--eval_dataset` | 必填 | 输入验证数据集路径 |
| `--output_path` | 必填 | 输出结果路径 |
| `--model_path` | 必填 | 模型路径 |
| `--n_samples` | 8 | 每个提示生成的样本数 |
| `--batch_size` | 128 | 批处理大小 |
| `--temperature` | 1.0 | 采样温度 |
| `--top_p` | 0.95 | Top-p 采样参数 |
| `--solve_threshold` | 0.99 | 判定为解决的阈值 |
| `--pass_at_k` | 1 | Pass@k 的 k 值 |
| `--rollout_mode` | async_vllm | 推理模式：sync/async_vllm/standalone_vllm |
| `--reward_server_url` | 环境变量 | KernelServer URL |
| `--reward_weights` | 0.3_0.4_0.3 | 编译/正确性/性能权重 |
| `--nnodes` | 1 | 节点数 |
| `--n_gpus_per_node` | 8 | 每节点 GPU 数 |

---

## 5. 关键指标解释及正常范围

### 5.1 核心评估指标

| 指标名称 | 计算方式 | 正常范围 | 说明 |
|---------|---------|---------|------|
| **score** | 加权综合分数 | 0.0 - 1.0 | 最终奖励分数，越高越好 |
| **correctness** | 布尔值 | True/False | 内核功能正确性 |
| **compilation** | 布尔值 | True/False | 编译是否成功 |
| **performance (speedup)** | 相对加速比 | ≥ 1.0 | 相对于 PyTorch 参考实现的加速 |
| **solve_rate** | 通过率 | 0.0 - 1.0 | 达到阈值的样本比例 |
| **pass@k** | 组合概率 | 0.0 - 1.0 | k 个样本中至少一个通过的概率 |

### 5.2 奖励计算公式

```
score = w_compilation * compilation_score 
      + w_correctness * correctness_score 
      + w_performance * performance_score

# 默认权重
w_compilation = 0.3
w_correctness = 0.4
w_performance = 0.3
```

### 5.3 各组件评分规则

**编译分数 (compilation_score)**

| 状态 | 分数 |
|------|------|
| 编译成功 | 1.0 |
| 编译失败 | -0.5 (惩罚) |

**正确性分数 (correctness_score)**

| 状态 | 分数 |
|------|------|
| 功能正确 | 1.0 |
| 功能错误 | -0.3 (惩罚) |

**性能分数 (performance_score)**

```python
# 加速比计算
speedup = reference_time / kernel_time

# 性能分数归一化
if speedup >= speedup_upper_bound:  # 默认 3.0
    performance_score = 1.0
elif speedup >= 1.0:
    performance_score = (speedup - 1.0) / (speedup_upper_bound - 1.0)
else:
    performance_score = -0.1  # 性能退化惩罚
```

### 5.4 多轮对话特有指标

| 指标 | 说明 | 正常范围 |
|------|------|---------|
| **num_turns** | 对话轮数 | 1 - max_user_turns |
| **final_turn_score** | 最终轮次分数 | 0.0 - 1.0 |
| **improvement** | 首末轮改进 | ≥ 0 表示改进 |
| **time_coverage** | 时间覆盖率 | 0.0 - 1.0 |
| **num_custom_kernel** | 自定义内核数 | ≥ 0 |
| **is_decoy_kernel** | 是否为欺骗内核 | False |

### 5.5 指标健康参考值

**SFT 训练后模型预期表现**

| 指标 | 初期模型 | 良好模型 | 优秀模型 |
|------|---------|---------|---------|
| solve_rate | 10-20% | 40-60% | > 70% |
| pass@1 | 0.1-0.2 | 0.4-0.6 | > 0.7 |
| avg_speedup | 1.0-1.2x | 1.5-2.0x | > 2.0x |
| compilation_rate | 60-70% | 80-90% | > 95% |
| correctness_rate | 30-40% | 60-75% | > 85% |

**Dr.Kernel 14B 参考基准**

```
# 单轮验证
solve_rate: ~45%
pass@1: ~0.45
avg_speedup: ~1.8x

# 多轮验证 (3 turns)
solve_rate: ~55%
pass@1: ~0.55
avg_speedup: ~2.1x
```

---

## 6. 常见错误排查方法

### 6.1 KernelServer 连接错误

**错误信息**
```
RuntimeError: KernelServer at http://xxx:9744 is not accessible
```

**排查步骤**

```bash
# 1. 检查服务是否运行
curl http://your-server:9744/health

# 2. 检查网络连通性
ping your-server
telnet your-server 9744

# 3. 检查防火墙设置
sudo ufw status
sudo iptables -L -n

# 4. 验证环境变量
echo $KERNELGYM_SERVER_URL
```

**解决方案**
- 确保 KernelServer 正在运行
- 检查 URL 配置是否正确
- 确认网络策略允许访问

### 6.2 GPU 内存不足

**错误信息**
```
torch.cuda.OutOfMemoryError: CUDA out of memory
```

**排查步骤**

```bash
# 1. 检查 GPU 使用情况
nvidia-smi

# 2. 检查当前进程
nvidia-smi --query-compute-apps=pid --format=csv

# 3. 清理缓存
python -c "import torch; torch.cuda.empty_cache()"
```

**解决方案**

```bash
# 降低 GPU 内存使用率
--rollout_gpu_memory_util 0.5

# 减小批处理大小
--batch_size 32

# 启用张量并行
--rollout_tp 2

# 使用多 GPU 分布
--n_gpus_per_node 4
```

### 6.3 模型加载失败

**错误信息**
```
FileNotFoundError: Model path not found
OSError: Can't load tokenizer
```

**排查步骤**

```bash
# 1. 验证模型路径
ls -la /models/drkernel-14b/

# 2. 检查必要文件
ls /models/drkernel-14b/config.json
ls /models/drkernel-14b/model.safetensors
ls /models/drkernel-14b/tokenizer.json

# 3. 测试模型加载
python -c "from transformers import AutoModel; AutoModel.from_pretrained('/models/drkernel-14b')"
```

**解决方案**
- 确认模型路径正确
- 检查文件权限
- 验证模型格式兼容性

### 6.4 Ray 初始化错误

**错误信息**
```
RuntimeError: Ray is not initialized
RayActorError: The actor died unexpectedly
```

**排查步骤**

```bash
# 1. 检查 Ray 状态
ray status

# 2. 停止现有 Ray 实例
ray stop

# 3. 清理 Ray 临时文件
rm -rf /tmp/ray/*
```

**解决方案**

```bash
# 重新初始化 Ray
ray stop
ray start --head

# 或在代码中
ray.init(ignore_reinit_error=True)
```

### 6.5 vLLM 引擎错误

**错误信息**
```
ValueError: vLLM engine failed to initialize
AssertionError: block_size must be divisible
```

**排查步骤**

```bash
# 1. 检查 vLLM 版本
pip show vllm

# 2. 验证 GPU 兼容性
python -c "import vllm; print(vllm.__version__)"

# 3. 测试基础推理
python -m vllm.entrypoints.llm --model /models/drkernel-14b
```

**解决方案**

```bash
# 启用 eager 模式
--rollout_enforce_eager True

# 调整内存利用率
--rollout_gpu_memory_util 0.7

# 禁用 chunked prefill
# 在配置中设置 enable_chunked_prefill: false
```

### 6.6 数据集加载错误

**错误信息**
```
FileNotFoundError: Dataset not found
ValueError: Invalid parquet file
```

**排查步骤**

```bash
# 1. 验证数据集格式
python -c "import pandas as pd; pd.read_parquet('data.parquet')"

# 2. 检查必要列
python -c "
import pandas as pd
df = pd.read_parquet('data.parquet')
print(df.columns.tolist())
print(df.head())
"

# 3. 验证 HuggingFace 数据集
python -c "from datasets import load_dataset; ds = load_dataset('hkust-nlp/drkernel-validation-data')"
```

**解决方案**
- 确认数据集包含必要列：`prompt`, `reward_model` (含参考代码)
- 验证 Parquet 文件完整性
- 检查 HuggingFace 访问权限

### 6.7 超时错误

**错误信息**
```
TimeoutError: Task timed out after 600 seconds
asyncio.TimeoutError
```

**排查步骤**

```bash
# 检查任务队列
# 查看 KernelServer 日志

# 调整超时参数
--reward_timeout 3600
--reward_task_timeout 1200
```

**解决方案**

```bash
# 增加超时时间
--reward_acquire_timeout 4800
--reward_timeout 3600
--reward_task_timeout 1200

# 减少并发数
--reward_max_concurrent 32
--reward_rate_limit 32
```

---

## 7. 验证结果输出格式及解读

### 7.1 输出文件结构

```
/output/grading_results/
├── graded_results.parquet      # 主要结果文件
├── raw_responses.jsonl         # 原始响应记录
├── metrics.json                # 汇总指标
├── eval_outputs/               # 详细评估输出
│   ├── problem_0_sample_0/     # 多轮对话目录
│   │   ├── turn_1_kernel.py    # 第1轮生成的内核代码
│   │   ├── turn_1_eval.json    # 第1轮评估结果
│   │   ├── turn_1_state.json   # 第1轮对话状态
│   │   ├── turn_2_kernel.py
│   │   ├── turn_2_eval.json
│   │   ├── reference.py        # 参考实现
│   │   ├── full_conversation.txt # 完整对话记录
│   │   └── summary.json        # 对话摘要
│   ├── problem_0_sample_1/
│   └── ...
├── gradio_url.txt              # Gradio 可视化 URL
└── conversations.jsonl         # 对话格式记录
```

### 7.2 graded_results.parquet 格式

| 列名 | 类型 | 说明 |
|------|------|------|
| `prompt` | string | 输入提示 |
| `solve_rate` | float | 解决率 (0.0-1.0) |
| `reward_model` | string/dict | 参考代码等信息 |
| `data_source` | string | 数据来源标识 |

### 7.3 metrics.json 格式

```json
{
  "val/test_score/kernel": 0.4523,
  "val/test_score/kernel_pass@1": 0.4523,
  "val/test_score_extra/correctness_kernel": 0.6234,
  "val/test_score_extra/performance_kernel": 1.8456,
  "val/test_score_extra/compilation_kernel": 0.8567,
  "val/test_score_extra/is_speedup_positive_kernel": 0.5123,
  "val/test_score_extra/time_coverage_kernel": 0.3456,
  "val/num_turns_mean": 2.8,
  "val/num_turns_std": 0.5,
  "val/final_turn_score_mean": 0.5234,
  "val/improvement_rate": 0.3456
}
```

### 7.4 turn_X_eval.json 格式

```json
{
  "score": 0.75,
  "problem_id": 42,
  "sample_id": 0,
  "turn_id": 2,
  "uid": "test_example_abc123",
  "correctness": true,
  "compilation": true,
  "performance": 2.15,
  "is_speedup_positive": true,
  "is_decoy_kernel": false,
  "num_custom_kernel": 3,
  "num_total_kernels": 5,
  "time_coverage": 0.65,
  "custom_kernel_cuda_time_in_profiling_us": 125000,
  "total_kernel_run_time_in_profiling_us": 192308,
  "finish_reason": "stop",
  "status": "success",
  "error": null,
  "reward_extra_info": {
    "correctness": true,
    "performance": 2.15,
    "compilation": true
  }
}
```

### 7.5 summary.json 格式

```json
{
  "uid": "test_example_abc123",
  "num_turns": 3,
  "total_score": 1.85,
  "per_turn_scores": [0.45, 0.65, 0.75],
  "problem_id": 42,
  "improvement": {
    "first_turn_score": 0.45,
    "last_turn_score": 0.75,
    "improved": true
  }
}
```

### 7.6 结果解读指南

**评估整体表现**

```python
import pandas as pd
import json

# 读取主要指标
with open('metrics.json') as f:
    metrics = json.load(f)

# 关键指标
print(f"平均分数: {metrics['val/test_score/kernel']:.4f}")
print(f"Pass@1: {metrics['val/test_score/kernel_pass@1']:.4f}")
print(f"正确率: {metrics['val/test_score_extra/correctness_kernel']:.4f}")
print(f"编译成功率: {metrics['val/test_score_extra/compilation_kernel']:.4f}")
print(f"平均加速比: {metrics['val/test_score_extra/performance_kernel']:.2f}x")
```

**分析多轮改进**

```python
import os
import json

eval_dir = 'eval_outputs'
improvements = []

for conv_dir in os.listdir(eval_dir):
    summary_path = os.path.join(eval_dir, conv_dir, 'summary.json')
    if os.path.exists(summary_path):
        with open(summary_path) as f:
            summary = json.load(f)
            if 'improvement' in summary:
                improvements.append(summary['improvement'])

# 统计改进情况
improved_count = sum(1 for imp in improvements if imp['improved'])
print(f"改进比例: {improved_count / len(improvements):.2%}")
print(f"平均首轮分数: {sum(imp['first_turn_score'] for imp in improvements) / len(improvements):.4f}")
print(f"平均末轮分数: {sum(imp['last_turn_score'] for imp in improvements) / len(improvements):.4f}")
```

**识别问题模式**

```python
import os
import json
from collections import Counter

eval_dir = 'eval_outputs'
error_patterns = Counter()

for conv_dir in os.listdir(eval_dir):
    for turn_file in os.listdir(os.path.join(eval_dir, conv_dir)):
        if turn_file.endswith('_eval.json'):
            with open(os.path.join(eval_dir, conv_dir, turn_file)) as f:
                eval_data = json.load(f)
                if eval_data.get('error'):
                    error_patterns[eval_data['error'][:50]] += 1

print("常见错误模式:")
for error, count in error_patterns.most_common(10):
    print(f"  {count}x: {error}")
```

### 7.7 Gradio 可视化界面

启动可视化后，可通过 Web 界面查看：

1. **样本选择器**: 下拉选择不同的 problem/sample 组合
2. **对话历史**: 显示完整的多轮对话过程
3. **代码对比**: 并排显示生成的内核代码和参考实现
4. **指标表格**: 展示各轮次的详细评估指标
5. **趋势分析**: 查看分数随轮次的改进情况

**访问方式**

```bash
# 本地访问
http://localhost:7860

# 公网访问（如果启用 share）
https://xxxxx.gradio.live

# URL 保存在
cat eval_outputs/gradio_url.txt
```

---

## 附录 A: 快速验证检查清单

```bash
# 1. 环境检查
conda activate drkernel
python -c "import torch; print(torch.cuda.is_available())"
python -c "import vllm; print(vllm.__version__)"

# 2. 服务检查
curl $KERNELGYM_SERVER_URL/health

# 3. 模型检查
ls -la $MODEL_PATH/config.json
ls -la $MODEL_PATH/model.safetensors

# 4. 数据检查
python -c "import pandas as pd; pd.read_parquet('$EVAL_DATASET')"

# 5. 执行验证
cd drkernel/kernel/scripts/eval
bash drkernel-14b-maxturns3.sh

# 6. 检查结果
cat /output/grading_results/metrics.json
```

## 附录 B: 参数调优建议

| 场景 | 推荐配置 |
|------|---------|
| 快速验证 | `n_samples=4, batch_size=64, temperature=0.8` |
| 高质量评估 | `n_samples=16, batch_size=128, temperature=1.0` |
| GPU 内存受限 | `rollout_gpu_memory_util=0.5, batch_size=32` |
| 多 GPU 环境 | `n_gpus_per_node=8, rollout_tp=2` |
| 调试模式 | `n_samples=1, batch_size=8, gradio=True` |

---

*文档版本: 1.0*
*最后更新: 2025-03*
