# Dr.Kernel项目完整流程复现指导

## 目录

1. [硬件环境确认与系统配置要求](#1-硬件环境确认与系统配置要求)
2. [操作系统版本、驱动程序及依赖库安装](#2-操作系统版本驱动程序及依赖库安装)

8. [常见问题排查与解决方案](#8-常见问题排查与解决方案)
9. [关键步骤验证方法与预期结果](#9-关键步骤验证方法与预期结果)
10. [完整命令行操作示例](#10-完整命令行操作示例)

---

## 1. 硬件环境确认与系统配置要求

### 1.1 硬件最低配置

| 组件 | 最低要求 | 推荐配置 |
|------|----------|----------|
| **GPU** | NVIDIA A100 40GB × 1 | NVIDIA A100 80GB × 8 |
| **CPU** | 16核心 | 32核心+ |
| **内存** | 128GB | 256GB+ |
| **存储** | 500GB SSD | 2TB NVMe SSD |
| **网络** | 10Gbps | 25Gbps (多节点训练) |

### 1.2 GPU环境确认

```bash
# 检查GPU型号
nvidia-smi --query-gpu=name,memory.total --format=csv

# 预期输出 (A100示例):
# name, memory.total [MiB]
# NVIDIA A100-SXM4-80GB, 81920 MiB

# 检查GPU数量
nvidia-smi -L | wc -l

# 检查GPU驱动版本
nvidia-smi --query-gpu=driver_version --format=csv,noheader
```

### 1.3 系统配置要求

```bash
# 检查操作系统
cat /etc/os-release

# 检查内核版本
uname -r

# 检查内存
free -h

# 检查磁盘空间
df -h
```

---

## 2. 操作系统版本、驱动程序及依赖库安装

### 2.1 操作系统要求

| 组件 | 版本要求 |
|------|----------|
| **操作系统** | Ubuntu 20.04/22.04 LTS |
| **Linux内核** | 5.4+ |
| **CUDA** | 12.1+ |
| **cuDNN** | 8.9+ |
| **NVIDIA驱动** | 535.104+ |

### 2.2 NVIDIA驱动安装

```bash
# 方法1: 使用apt安装 (推荐)
sudo apt update
sudo apt install -y nvidia-driver-535
sudo reboot

# 方法2: 使用官方.run文件
wget https://us.download.nvidia.com/tesla/535.154.05/NVIDIA-Linux-x86_64-535.154.05.run
sudo sh NVIDIA-Linux-x86_64-535.154.05.run --silent

# 验证安装
nvidia-smi
```

### 2.3 CUDA Toolkit安装

```bash
# 安装CUDA 12.1
wget https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2204/x86_64/cuda-keyring_1.1-1_all.deb
sudo dpkg -i cuda-keyring_1.1-1_all.deb
sudo apt update
sudo apt install -y cuda-12-1

# 配置环境变量
echo 'export PATH=/usr/local/cuda/bin:$PATH' >> ~/.bashrc
echo 'export LD_LIBRARY_PATH=/usr/local/cuda/lib64:$LD_LIBRARY_PATH' >> ~/.bashrc
source ~/.bashrc

# 验证安装
nvcc --version
```

### 2.4 Python环境安装

```bash
# 安装Miniconda
wget https://repo.anaconda.com/miniconda/Miniconda3-latest-Linux-x86_64.sh
bash Miniconda3-latest-Linux-x86_64.sh -b -p $HOME/miniconda3
source ~/miniconda3/etc/profile.d/conda.sh

# 创建Python 3.10环境
conda create -n drkernel python=3.10 -y
conda activate drkernel

# 验证Python版本
python --version  # 应输出 Python 3.10.x
```

### 2.5 Redis安装 (用于KernelGYM任务队列)

```bash
# 安装Redis
sudo apt install -y redis-server

# 启动Redis服务
sudo systemctl start redis-server
sudo systemctl enable redis-server

# 验证Redis运行
redis-cli ping  # 应输出 PONG
```

### 2.6 其他系统依赖

```bash
# 安装编译工具和依赖
sudo apt install -y \
    build-essential \
    git \
    git-lfs \
    wget \
    curl \
    vim \
    htop \
    tmux \
    libssl-dev \
    libffi-dev \
    python3-dev \
    pkg-config

# 初始化Git LFS
git lfs install
```

---

## 3. 项目源代码获取与版本控制

### 3.1 克隆项目仓库

```bash
# 创建工作目录
mkdir -p ~/projects && cd ~/projects

# 克隆KernelGYM仓库 (包含Dr.Kernel)
git clone https://github.com/hkust-nlp/KernelGYM.git
cd KernelGYM

# 查看当前版本
git log -1 --oneline

# 初始化子模块 (VERL框架)
git submodule update --init --recursive
```

### 3.2 项目目录结构

```
KernelGYM/
├── kernelgym/                    # 核心评估环境
│   ├── backend/                  # 后端抽象层
│   ├── toolkit/                  # 工具包
│   ├── server/                   # API服务
│   ├── worker/                   # GPU Worker
│   └── config/                   # 配置
├── drkernel/                     # Dr.Kernel训练框架
│   ├── kernel/                   # 核心训练模块
│   │   ├── scripts/              # 训练脚本
│   │   │   ├── rl/               # RL训练脚本
│   │   │   ├── sft/              # SFT训练脚本
│   │   │   └── eval/             # 评估脚本
│   │   └── main_kernel.py        # 主入口
│   ├── verl_patch/               # VERL框架补丁
│   └── setup.sh                  # 安装脚本
├── verl/                         # VERL子模块
├── scripts/                      # 部署脚本
└── requirements.txt              # 依赖列表
```

---

## 4. 编译环境搭建与编译参数配置

### 4.1 安装Python依赖

```bash
# 激活环境
conda activate drkernel
cd ~/projects/KernelGYM

# 安装基础依赖
pip install --upgrade pip setuptools wheel

# 安装PyTorch (CUDA 12.1版本)
pip install torch==2.8.0 torchvision==0.23.0 torchaudio==2.8.0 --index-url https://download.pytorch.org/whl/cu121

# 或者使用项目提供的setup脚本
cd drkernel
bash setup.sh
```

### 4.2 安装VERL框架

```bash
cd ~/projects/KernelGYM

# 进入VERL子模块目录
cd verl

# 安装VERL (可编辑模式)
pip install -e . --no-build-isolation --no-deps

# 安装Ray
pip install --no-cache-dir "ray==2.47.1"
```

### 4.3 安装vLLM推理引擎

```bash
# 安装vLLM
pip install --no-cache-dir "vllm==0.10.2"

# 验证vLLM安装
python -c "import vllm; print(vllm.__version__)"
```

### 4.4 安装Flash Attention

```bash
# 安装Flash Attention 2.8.3
# 根据CUDA版本选择ABI
ABI_FLAG="FALSE"  # 或 "TRUE" 取决于PyTorch编译选项

URL="https://github.com/Dao-AILab/flash-attention/releases/download/v2.8.3/flash_attn-2.8.3+cu12torch2.8cxx11abi${ABI_FLAG}-cp310-cp310-linux_x86_64.whl"
wget -nv -P . "${URL}"
pip install --no-cache-dir "./$(basename "${URL}")"

# 验证安装
python -c "import flash_attn; print(flash_attn.__version__)"
```

### 4.5 安装其他依赖

```bash
# 安装transformers和其他ML库
pip install --no-cache-dir \
    "transformers[hf_xet]==4.56.0" \
    accelerate \
    datasets \
    peft \
    hf-transfer \
    "numpy<2.0.0" \
    "pyarrow>=15.0.0" \
    pandas \
    hydra-core \
    wandb==0.16.6

# 安装其他工具
pip install sandbox-fusion --user
pip install logfire --user
pip install gradio --user
pip install huggingface_hub --user
pip install protobuf==3.20 --user
```

### 4.6 验证安装

```bash
# 运行验证脚本
python -c "
import torch
import vllm
import transformers
import ray

print(f'PyTorch: {torch.__version__}')
print(f'CUDA available: {torch.cuda.is_available()}')
print(f'CUDA version: {torch.version.cuda}')
print(f'GPU count: {torch.cuda.device_count()}')
print(f'vLLM: {vllm.__version__}')
print(f'Transformers: {transformers.__version__}')
print(f'Ray: {ray.__version__}')

if torch.cuda.is_available():
    for i in range(torch.cuda.device_count()):
        print(f'GPU {i}: {torch.cuda.get_device_name(i)}')
"
```

---

## 5. 数据集准备、预处理及存放路径设置

### 5.1 数据集概述

Dr.Kernel使用以下数据集：

| 数据集 | 用途 | HuggingFace路径 |
|--------|------|-----------------|
| **drkernel-coldstart-8k** | SFT冷启动训练 | `hkust-nlp/drkernel-coldstart-8k` |
| **drkernel-rl-data** | RL训练 | `hkust-nlp/drkernel-rl-data` |
| **drkernel-validation-data** | 验证集 | `hkust-nlp/drkernel-validation-data` |
| **KernelBench** | 评估基准 | `hkust-nlp/KernelBench` |

### 5.2 下载数据集

```bash
# 创建数据目录
mkdir -p ~/projects/KernelGYM/data
cd ~/projects/KernelGYM/data

# 方法1: 使用huggingface-cli下载
pip install huggingface_hub
huggingface-cli login  # 如果需要，输入HuggingFace token

# 下载SFT数据集
huggingface-cli download hkust-nlp/drkernel-coldstart-8k \
    --repo-type dataset \
    --local-dir ./drkernel-coldstart-8k

# 下载RL训练数据集
huggingface-cli download hkust-nlp/drkernel-rl-data \
    --repo-type dataset \
    --local-dir ./drkernel-rl-data

# 下载验证数据集
huggingface-cli download hkust-nlp/drkernel-validation-data \
    --repo-type dataset \
    --local-dir ./drkernel-validation-data

# 方法2: 使用Python脚本下载
python -c "
from datasets import load_dataset
ds = load_dataset('hkust-nlp/drkernel-coldstart-8k')
print(ds)
"
```

### 5.3 数据集格式说明

```python
# SFT数据集格式
{
    "prompt": "Optimize the following PyTorch kernel...",
    "response": "Here's an optimized Triton kernel..."
}

# RL训练数据集格式
{
    "id": "kernel_001",
    "prompt": "Write a Triton kernel for softmax...",
    "reference_code": "def softmax(x): ...",
    "test_inputs": {...},
    "metadata": {...}
}
```

### 5.4 数据路径配置

```bash
# 设置环境变量
export HDFS_DATA_PATH=~/projects/KernelGYM/data
export HDFS_MODEL_PATH=~/projects/KernelGYM/models
export HDFS_CHECKPOINT_PATH=~/projects/KernelGYM/checkpoints

# 创建必要目录
mkdir -p $HDFS_MODEL_PATH
mkdir -p $HDFS_CHECKPOINT_PATH
```

### 5.5 下载预训练模型

```bash
# 下载Qwen3-8B-Base模型 (SFT基础模型)
huggingface-cli download Qwen/Qwen2.5-7B \
    --local-dir $HDFS_MODEL_PATH/Qwen2.5-7B

# 或下载Dr.Kernel预训练模型
huggingface-cli download hkust-nlp/drkernel-8b \
    --local-dir $HDFS_MODEL_PATH/drkernel-8b
```

---

## 6. 模型训练完整流程

### 6.1 训练流程概览

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                        Dr.Kernel 训练流程                                    │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│  Phase 1: SFT Cold Start                                                     │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │ - 数据集: drkernel-coldstart-8k                                      │   │
│  │ - 基础模型: Qwen2.5-7B-Base                                          │   │
│  │ - 训练轮次: 4 epochs                                                 │   │
│  │ - 学习率: 2e-5                                                       │   │
│  │ - 输出: drkernel-8b-coldstart                                        │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│                                      │                                       │
│                                      ▼                                       │
│  Phase 2: RL Training (TRLOO + MRS + PR + PRS)                               │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │ - 数据集: drkernel-rl-data                                           │   │
│  │ - 初始模型: drkernel-8b-coldstart                                    │   │
│  │ - 算法: TRLOO (Turn-level REINFORCE Leave-One-Out)                   │   │
│  │ - 特性: MRS (Multi-turn Rejection Sampling)                          │   │
│  │        PR (Profiling-based Rewards)                                  │   │
│  │        PRS (Profiling-based Rejection Sampling)                      │   │
│  │ - 训练轮次: 1000 epochs                                              │   │
│  │ - 学习率: 1e-6                                                       │   │
│  │ - 输出: drkernel-8b                                                  │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│                                                                              │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 6.2 Phase 1: SFT冷启动训练

#### 6.2.1 配置SFT训练参数

```bash
cd ~/projects/KernelGYM/drkernel/kernel/scripts/sft

# 编辑配置文件 (可选)
vim 8b-coldstart.sh
```

#### 6.2.2 关键SFT参数说明

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `TRAIN_BATCH_SIZE` | 64 | 训练批次大小 |
| `MICRO_BATCH_SIZE_PER_GPU` | 2 | 每GPU微批次大小 |
| `MAX_LENGTH` | 18432 | 最大序列长度 |
| `TOTAL_EPOCHS` | 4 | 训练轮次 |
| `LEARNING_RATE` | 2e-5 | 学习率 |
| `SP_SIZE` | 4 | Ulysses序列并行大小 |

#### 6.2.3 执行SFT训练

```bash
# 单节点8卡训练
cd ~/projects/KernelGYM/drkernel/kernel/scripts/sft

# 设置环境变量
export HDFS_MODEL_PATH=~/projects/KernelGYM/models
export HDFS_CHECKPOINT_PATH=~/projects/KernelGYM/checkpoints
export HDFS_DATA_PATH=~/projects/KernelGYM/data

# 运行SFT训练
bash 8b-coldstart.sh

# 或自定义参数运行
bash 8b-coldstart.sh \
    --train_batch_size 32 \
    --learning_rate 1e-5 \
    --total_epochs 2
```

#### 6.2.4 SFT训练预期输出

```
RUN_NAME: drkernel-8b-coldstart_batch64_micro2_maxlen18432_epochs4_lr2e-5
Training with the following parameters:
Train Batch Size: 64
Micro Batch Size per GPU: 2
Max Length: 18432
Total Epochs: 4
Learning Rate: 2e-5
...
Step 100/1000: loss=0.523, lr=2.0e-05
Step 200/1000: loss=0.412, lr=1.9e-05
...
```

### 6.3 Phase 2: RL训练

#### 6.3.1 启动KernelGYM评估服务

```bash
# 终端1: 启动Redis (如果未运行)
sudo systemctl start redis-server

# 终端2: 启动KernelGYM API服务
cd ~/projects/KernelGYM

# 自动配置环境
bash scripts/auto_configure.sh --force

# 加载环境变量
source .env

# 启动API服务
python -m kernelgym.server.api.server

# 或使用uvicorn启动
uvicorn kernelgym.server.api.server:app \
    --host 0.0.0.0 \
    --port 10907 \
    --workers 4
```

#### 6.3.2 启动GPU Worker

```bash
# 终端3: 启动GPU Worker
cd ~/projects/KernelGYM
source .env

# 启动Worker (假设使用GPU 0)
CUDA_VISIBLE_DEVICES=0 python -m kernelgym.worker.gpu_worker \
    --device cuda:0 \
    --node-id node-0

# 多GPU Worker (使用多个终端)
CUDA_VISIBLE_DEVICES=0 python -m kernelgym.worker.gpu_worker --device cuda:0 &
CUDA_VISIBLE_DEVICES=1 python -m kernelgym.worker.gpu_worker --device cuda:1 &
# ... 更多GPU
```

#### 6.3.3 验证服务状态

```bash
# 检查API服务健康状态
curl http://localhost:10907/health

# 预期输出:
# {"status": "healthy", "redis": "connected", "workers": 8}

# 检查Worker状态
curl http://localhost:10907/workers/status

# 测试评估接口
curl -X POST http://localhost:10907/evaluate \
    -H "Content-Type: application/json" \
    -d '{
        "task_id": "test-001",
        "reference_code": "def softmax(x): return x",
        "kernel_code": "def softmax(x): return x",
        "entry_point": "softmax"
    }'
```

#### 6.3.4 配置RL训练参数

```bash
cd ~/projects/KernelGYM/drkernel/kernel/scripts/rl

# 查看训练脚本
cat 8b_trloo_mrs_pr_prs.sh
```

#### 6.3.5 关键RL参数说明

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `ALGORITHM` | trloo | TRLOO算法 |
| `TRAIN_BATCH_SIZE` | 16 | 训练批次大小 |
| `LEARNING_RATE` | 1e-6 | 学习率 |
| `MAX_TURN` | 3 | 多轮对话最大轮次 |
| `ROLLOUT_N` | 16 | 每个prompt生成的响应数 |
| `COVERAGE_REWARD_WEIGHT` | 0.5 | 覆盖率奖励权重 |
| `COVERAGE_REWARD_TYPE` | time_coverage | 覆盖率类型 |
| `SPEEDUP_REWARD_UPPER_BOUND` | 3.0 | 加速比奖励上限 |
| `NUM_PERF_TRIALS` | 100 | 性能测试试验次数 |

#### 6.3.6 执行RL训练

```bash
# 设置KernelGYM服务URL
export KERNELGYM_SERVER_URL="http://localhost:10907"

# 运行RL训练
cd ~/projects/KernelGYM/drkernel/kernel/scripts/rl
bash 8b_trloo_mrs_pr_prs.sh

# 或自定义参数
bash 8b_trloo_mrs_pr_prs.sh \
    --train_batch_size 8 \
    --learning_rate 5e-7 \
    --max_turn 2
```

#### 6.3.7 RL训练预期输出

```
RUN_NAME: drkernel-8b_abc123
Training with the following parameters:
Train Batch Size: 16
Learning Rate: 1e-06
PPO Mini Batch Size: 16
Algorithm: trloo
Max Turn: 3
...

Epoch 1/1000:
  Rollout: 100%|████████| 16/16 [02:30<00:00]
  Reward: mean=0.45, std=0.23
  Coverage: mean=0.32, std=0.15
  Speedup: mean=1.8, std=0.5
  Loss: actor=0.023, critic=0.045

Epoch 10/1000:
  Checkpoint saved to checkpoints/drkernel-8b/epoch_10
  Validation: accuracy=0.65, avg_speedup=2.1
...
```

### 6.4 参数调优建议

#### 6.4.1 SFT阶段调优

| 场景 | 建议调整 |
|------|----------|
| 显存不足 | 降低`MICRO_BATCH_SIZE_PER_GPU`或`MAX_LENGTH` |
| 训练不稳定 | 降低`LEARNING_RATE`至1e-5 |
| 过拟合 | 增加`TRAIN_BATCH_SIZE`或添加正则化 |
| 欠拟合 | 增加`TOTAL_EPOCHS`或提高`LEARNING_RATE` |

#### 6.4.2 RL阶段调优

| 场景 | 建议调整 |
|------|----------|
| 奖励稀疏 | 增加`COVERAGE_REWARD_WEIGHT` |
| 训练不稳定 | 降低`LEARNING_RATE`至5e-7 |
| 奖励黑客 | 启用`DETECT_DECOY_KERNEL=True` |
| 性能差 | 增加`NUM_PERF_TRIALS`至200 |
| 多轮效果差 | 调整`MAX_TURN`和`GAMMA` |

---

## 7. 模型评估指标与评估方法

### 7.1 评估指标

| 指标 | 说明 | 计算方式 |
|------|------|----------|
| **Compilation Rate** | 编译成功率 | 成功编译数 / 总提交数 |
| **Correctness Rate** | 正确性通过率 | 正确数 / 编译成功数 |
| **Average Speedup** | 平均加速比 | mean(kernel_time / reference_time) |
| **Coverage** | 内核覆盖率 | custom_kernel_time / total_time |
| **Decoy Rate** | 诱饵内核检测率 | decoy数 / 正确数 |
| **Greedy Accuracy** | 贪婪解码准确率 | 正确数 / 总数 (temperature=0) |

### 7.2 评估脚本

```bash
cd ~/projects/KernelGYM/drkernel/kernel/scripts/eval

# 查看评估脚本
ls -la
# 输出:
# drkernel-14b-maxturns3.sh
# grading_common.sh
# ...

# 运行评估
bash drkernel-14b-maxturns3.sh
```

### 7.3 KernelBench评估

```bash
# 下载KernelBench数据集
huggingface-cli download hkust-nlp/KernelBench \
    --repo-type dataset \
    --local-dir ~/projects/KernelGYM/data/KernelBench

# 运行KernelBench评估
cd ~/projects/KernelGYM

python -m kernelgym.toolkit.kernelbench.evaluate \
    --model_path checkpoints/drkernel-8b/final \
    --benchmark_path data/KernelBench \
    --output_path results/kernelbench_results.json \
    --num_trials 100 \
    --device cuda:0
```

### 7.4 评估结果示例

```json
{
    "model": "drkernel-8b",
    "benchmark": "KernelBench",
    "results": {
        "level_1": {
            "total": 150,
            "compiled": 145,
            "correct": 138,
            "avg_speedup": 2.35,
            "coverage": 0.42
        },
        "level_2": {
            "total": 100,
            "compiled": 92,
            "correct": 85,
            "avg_speedup": 1.89,
            "coverage": 0.38
        }
    },
    "overall": {
        "compilation_rate": 0.95,
        "correctness_rate": 0.92,
        "avg_speedup": 2.12,
        "greedy_accuracy": 0.68
    }
}
```

---

## 8. 常见问题排查与解决方案

### 8.1 CUDA相关错误

#### 问题1: CUDA Out of Memory

```bash
# 症状
RuntimeError: CUDA out of memory. Tried to allocate X.XX GiB

# 解决方案
# 1. 降低批次大小
export TRAIN_BATCH_SIZE=8
export PPO_MINI_BATCH_SIZE=8

# 2. 启用梯度检查点
export MODEL_ENABLE_GRADIENT_CHECKPOINTING=True

# 3. 启用参数卸载
export ACTOR_PARAMETER_OFFLOAD=True
export ACTOR_OPTIMIZER_OFFLOAD=True

# 4. 降低vLLM显存占用
export ROLLOUT_GPU_MEMORY_UTIL=0.5
```

#### 问题2: CUDA Error: Device-side assert triggered

```bash
# 症状
CUDA error: device-side assert triggered

# 解决方案
# 1. 重启GPU Worker
pkill -f gpu_worker
python -m kernelgym.worker.gpu_worker --device cuda:0

# 2. 检查内核代码正确性
# 使用CPU模式测试
export CUDA_LAUNCH_BLOCKING=1
```

### 8.2 Redis连接错误

#### 问题: Redis连接超时

```bash
# 症状
redis.exceptions.ConnectionError: Error connecting to Redis

# 解决方案
# 1. 检查Redis服务状态
sudo systemctl status redis-server

# 2. 重启Redis
sudo systemctl restart redis-server

# 3. 检查Redis配置
redis-cli config get bind
# 应该包含 127.0.0.1 或 0.0.0.0

# 4. 检查端口
netstat -tlnp | grep 6379
```

### 8.3 vLLM相关错误

#### 问题: vLLM初始化失败

```bash
# 症状
ValueError: vLLM initialization failed

# 解决方案
# 1. 检查GPU显存
nvidia-smi

# 2. 降低显存占用
export ROLLOUT_GPU_MEMORY_UTIL=0.5
export VLLM_ATTENTION_BACKEND=FLASHINFER

# 3. 使用tensor parallel
export ROLLOUT_TENSOR_MODEL_PARALLEL_SIZE=2
```

### 8.4 训练相关错误

#### 问题: NaN Loss

```bash
# 症状
RuntimeError: NaN detected in loss

# 解决方案
# 1. 降低学习率
export LEARNING_RATE=5e-7

# 2. 启用梯度裁剪
export GRAD_CLIP=0.5

# 3. 检查数据质量
# 查看是否有异常数据
python -c "
import pandas as pd
df = pd.read_parquet('data/drkernel-rl-data/train.parquet')
print(df.describe())
"
```

#### 问题: 训练卡住

```bash
# 症状
训练进程无响应，日志停止输出

# 解决方案
# 1. 检查Ray状态
ray status

# 2. 重启Ray集群
ray stop
ray start --head

# 3. 检查Worker状态
curl http://localhost:10907/workers/status

# 4. 检查任务队列
redis-cli -c "llen kernelgym:queue:priority:normal"
```

### 8.5 性能问题

#### 问题: 评估速度慢

```bash
# 解决方案
# 1. 增加Worker数量
for i in {0..7}; do
    CUDA_VISIBLE_DEVICES=$i python -m kernelgym.worker.gpu_worker --device cuda:$i &
done

# 2. 增加并发数
export REWARD_MAX_CONCURRENT=64

# 3. 减少性能测试次数
export NUM_PERF_TRIALS=50
```

---

## 9. 关键步骤验证方法与预期结果

### 9.1 环境验证

```bash
# 验证脚本
cat > verify_environment.sh << 'EOF'
#!/bin/bash
echo "=== Dr.Kernel Environment Verification ==="

# 1. GPU检查
echo -e "\n[1/6] GPU Check:"
nvidia-smi --query-gpu=name,memory.total,memory.free --format=csv

# 2. CUDA检查
echo -e "\n[2/6] CUDA Check:"
nvcc --version
python -c "import torch; print(f'PyTorch CUDA: {torch.cuda.is_available()}')"

# 3. Python环境检查
echo -e "\n[3/6] Python Environment:"
python -c "
import torch, vllm, transformers, ray
print(f'PyTorch: {torch.__version__}')
print(f'vLLM: {vllm.__version__}')
print(f'Transformers: {transformers.__version__}')
print(f'Ray: {ray.__version__}')
"

# 4. Redis检查
echo -e "\n[4/6] Redis Check:"
redis-cli ping

# 5. Flash Attention检查
echo -e "\n[5/6] Flash Attention Check:"
python -c "import flash_attn; print(f'Flash Attention: {flash_attn.__version__}')"

# 6. 数据集检查
echo -e "\n[6/6] Dataset Check:"
ls -la ~/projects/KernelGYM/data/

echo -e "\n=== Verification Complete ==="
EOF

chmod +x verify_environment.sh
./verify_environment.sh
```

### 9.2 SFT训练验证

```bash
# 验证SFT训练结果
ls -la $HDFS_CHECKPOINT_PATH/drkernel-8b-coldstart/

# 预期输出:
# config.json
# pytorch_model.bin (或 model.safetensors)
# tokenizer.json
# trainer_state.json
# ...

# 测试SFT模型
python -c "
from transformers import AutoModelForCausalLM, AutoTokenizer
model = AutoModelForCausalLM.from_pretrained(
    'checkpoints/drkernel-8b-coldstart',
    device_map='auto'
)
tokenizer = AutoTokenizer.from_pretrained('checkpoints/drkernel-8b-coldstart')
prompt = 'Write a Triton kernel for softmax:'
inputs = tokenizer(prompt, return_tensors='pt').to('cuda')
outputs = model.generate(**inputs, max_length=100)
print(tokenizer.decode(outputs[0]))
"
```

### 9.3 RL训练验证

```bash
# 验证RL训练结果
ls -la $HDFS_CHECKPOINT_PATH/drkernel-8b/

# 检查训练日志
tail -100 logs/drkernel-8b.log

# 检查WandB日志
wandb login
# 访问 https://wandb.ai 查看训练曲线

# 测试RL模型
python -c "
from transformers import AutoModelForCausalLM, AutoTokenizer
model = AutoModelForCausalLM.from_pretrained(
    'checkpoints/drkernel-8b/final',
    device_map='auto'
)
tokenizer = AutoTokenizer.from_pretrained('checkpoints/drkernel-8b/final')

prompt = '''Optimize the following PyTorch kernel:
def softmax(x):
    return torch.softmax(x, dim=-1)
'''
inputs = tokenizer(prompt, return_tensors='pt').to('cuda')
outputs = model.generate(**inputs, max_length=500, temperature=0.7)
print(tokenizer.decode(outputs[0]))
"
```

### 9.4 KernelGYM服务验证

```bash
# 验证API服务
curl -s http://localhost:10907/health | jq

# 预期输出:
# {
#   "status": "healthy",
#   "redis": "connected",
#   "workers": 8,
#   "queue_pending": 0
# }

# 验证评估功能
curl -X POST http://localhost:10907/evaluate \
    -H "Content-Type: application/json" \
    -d '{
        "task_id": "verify-001",
        "reference_code": "import torch\ndef softmax(x):\n    return torch.softmax(x, dim=-1)",
        "kernel_code": "import triton\nimport triton.language as tl\n@triton.jit\ndef softmax_kernel(x_ptr, output_ptr, n_elements, BLOCK_SIZE: tl.constexpr):\n    pass",
        "entry_point": "softmax",
        "num_perf_trials": 10
    }' | jq

# 预期输出包含:
# {
#   "task_id": "verify-001",
#   "compiled": true/false,
#   "correctness": true/false,
#   "speedup": X.XX,
#   ...
# }
```

---

## 10. 完整命令行操作示例

### 10.1 单GPU快速开始 (A100 40GB/80GB)

```bash
#!/bin/bash
# 完整训练流程 - 单GPU版本

# ===== 1. 环境准备 =====
conda activate drkernel
cd ~/projects/KernelGYM

# ===== 2. 启动服务 =====
# 终端1: Redis
sudo systemctl start redis-server

# 终端2: KernelGYM API
bash scripts/auto_configure.sh --force
source .env
python -m kernelgym.server.api.server &
sleep 10

# 终端3: GPU Worker
CUDA_VISIBLE_DEVICES=0 python -m kernelgym.worker.gpu_worker --device cuda:0 &
sleep 10

# ===== 3. 验证服务 =====
curl http://localhost:10907/health

# ===== 4. SFT训练 =====
cd ~/projects/KernelGYM/drkernel/kernel/scripts/sft
export HDFS_MODEL_PATH=~/projects/KernelGYM/models
export HDFS_CHECKPOINT_PATH=~/projects/KernelGYM/checkpoints
export HDFS_DATA_PATH=~/projects/KernelGYM/data

# 下载基础模型
huggingface-cli download Qwen/Qwen2.5-7B \
    --local-dir $HDFS_MODEL_PATH/Qwen2.5-7B

# 运行SFT (约2-4小时)
bash 8b-coldstart.sh --train_batch_size 16 --total_epochs 2

# ===== 5. RL训练 =====
cd ~/projects/KernelGYM/drkernel/kernel/scripts/rl
export KERNELGYM_SERVER_URL="http://localhost:10907"

# 运行RL训练 (约10-20小时/100 epochs)
bash 8b_trloo_mrs_pr_prs.sh

# ===== 6. 评估 =====
cd ~/projects/KernelGYM
python -m kernelgym.toolkit.kernelbench.evaluate \
    --model_path checkpoints/drkernel-8b/final \
    --benchmark_path data/KernelBench \
    --output_path results/eval_results.json
```

### 10.2 多GPU训练 (8×A100)

```bash
#!/bin/bash
# 完整训练流程 - 8 GPU版本

# ===== 1. 环境准备 =====
conda activate drkernel
cd ~/projects/KernelGYM

# ===== 2. 启动服务 =====
# 启动Redis
sudo systemctl start redis-server

# 自动配置
bash scripts/auto_configure.sh --force
source .env

# 启动API服务
python -m kernelgym.server.api.server &
sleep 5

# 启动所有GPU Worker
for i in {0..7}; do
    CUDA_VISIBLE_DEVICES=$i python -m kernelgym.worker.gpu_worker \
        --device cuda:$i --node-id node-0 &
done
sleep 10

# ===== 3. 验证服务 =====
curl http://localhost:10907/health
curl http://localhost:10907/workers/status

# ===== 4. SFT训练 =====
cd ~/projects/KernelGYM/drkernel/kernel/scripts/sft
export HDFS_MODEL_PATH=~/projects/KernelGYM/models
export HDFS_CHECKPOINT_PATH=~/projects/KernelGYM/checkpoints
export HDFS_DATA_PATH=~/projects/KernelGYM/data
export GPUS_PER_NODE=8
export NNODES=1

# 运行SFT (约1-2小时)
bash 8b-coldstart.sh

# ===== 5. RL训练 =====
cd ~/projects/KernelGYM/drkernel/kernel/scripts/rl
export KERNELGYM_SERVER_URL="http://localhost:10907"
export GPUS_PER_NODE=8
export NNODES=1

# 运行RL训练 (约5-10小时/100 epochs)
bash 8b_trloo_mrs_pr_prs.sh

# ===== 6. 监控训练 =====
# 查看日志
tail -f logs/drkernel-8b.log

# 查看GPU使用
watch -n 1 nvidia-smi

# 查看队列状态
watch -n 5 'curl -s http://localhost:10907/queue/status | jq'
```

### 10.3 多节点训练 (2节点 × 8GPU)

```bash
#!/bin/bash
# 完整训练流程 - 多节点版本

# ===== 主节点 (Node 0) =====
# 1. 启动Redis
sudo systemctl start redis-server

# 2. 启动API服务
bash scripts/auto_configure.sh --force
source .env
python -m kernelgym.server.api.server &

# 3. 启动Worker
for i in {0..7}; do
    CUDA_VISIBLE_DEVICES=$i python -m kernelgym.worker.gpu_worker \
        --device cuda:$i --node-id node-0 &
done

# ===== 工作节点 (Node 1) =====
# 在Node 1上执行
export API_HOST=<主节点IP>
export REDIS_HOST=<主节点IP>

# 启动Worker
for i in {0..7}; do
    CUDA_VISIBLE_DEVICES=$i python -m kernelgym.worker.gpu_worker \
        --device cuda:$i --node-id node-1 &
done

# ===== 训练 (在主节点执行) =====
cd ~/projects/KernelGYM/drkernel/kernel/scripts/rl
export KERNELGYM_SERVER_URL="http://localhost:10907"
export GPUS_PER_NODE=8
export NNODES=2
export MASTER_ADDR=<主节点IP>
export NODE_RANK=0  # 主节点为0，工作节点为1

bash 8b_trloo_mrs_pr_prs.sh
```

### 10.4 快速测试命令

```bash
# 快速测试 - 仅验证流程是否正常

# 1. 测试KernelGYM评估
curl -X POST http://localhost:10907/evaluate \
    -H "Content-Type: application/json" \
    -d '{
        "task_id": "quick-test",
        "reference_code": "import torch\ndef add(a, b): return a + b",
        "kernel_code": "import torch\ndef add(a, b): return a + b",
        "entry_point": "add",
        "num_perf_trials": 5
    }'

# 2. 测试SFT训练 (1 epoch)
cd ~/projects/KernelGYM/drkernel/kernel/scripts/sft
bash 8b-coldstart.sh --total_epochs 1 --train_batch_size 4

# 3. 测试RL训练 (1 epoch)
cd ~/projects/KernelGYM/drkernel/kernel/scripts/rl
bash 8b_trloo_mrs_pr_prs.sh --total_epochs 1 --train_batch_size 4
```

---

## 附录A: 配置文件模板

### A.1 .env配置文件

```bash
# KernelGYM配置
API_HOST=0.0.0.0
API_PORT=10907
GPU_DEVICES=[0,1,2,3,4,5,6,7]
NODE_ID=node-0

# Redis配置
REDIS_HOST=localhost
REDIS_PORT=6379
REDIS_DB=0
REDIS_KEY_PREFIX=kernelgym

# Worker配置
WORKER_POOL_SIZE=1
MAX_TASKS_PER_WORKER=1

# 默认配置
DEFAULT_TOOLKIT=kernelbench
DEFAULT_BACKEND_ADAPTER=kernelbench
DEFAULT_BACKEND=triton

# 日志配置
LOG_LEVEL=INFO
LOG_DIR=logs
```

### A.2 WandB配置

```bash
# 登录WandB
wandb login

# 或设置环境变量
export WANDB_API_KEY=your_api_key
export WANDB_PROJECT=drkernel
export WANDB_ENTITY=your_entity
```

---

## 附录B: 性能基准

### B.1 单GPU (A100 80GB) 性能

| 阶段 | 时间 | 显存占用 |
|------|------|----------|
| SFT (4 epochs) | ~4小时 | ~60GB |
| RL (100 epochs) | ~15小时 | ~70GB |
| 评估 | ~30分钟 | ~10GB |

### B.2 8 GPU (A100 80GB) 性能

| 阶段 | 时间 | 显存占用/GPU |
|------|------|--------------|
| SFT (4 epochs) | ~1小时 | ~40GB |
| RL (100 epochs) | ~5小时 | ~50GB |
| 评估 | ~10分钟 | ~10GB |

---

## 附录C: 参考资源

- **项目仓库**: https://github.com/hkust-nlp/KernelGYM
- **论文**: Dr.Kernel: Reinforcement Learning Done Right for Triton Kernel Generations
- **VERL框架**: https://github.com/volcengine/verl
- **vLLM文档**: https://vllm.readthedocs.io/
- **Triton文档**: https://triton-lang.org/
- **KernelBench**: https://huggingface.co/datasets/hkust-nlp/KernelBench
