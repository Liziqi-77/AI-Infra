# KernelGYM脚本文件详细解析文档

## 目录

1. [start_all_with_monitor.sh 详细解析](#1-start_all_with_monitorsh-详细解析)
2. [start_worker_node.sh 详细解析](#2-start_worker_nodesh-详细解析)
3. [训练脚本目录详细解析](#3-训练脚本目录详细解析)
   - [RL训练脚本](#31-rl训练脚本)
   - [SFT训练脚本](#32-sft训练脚本)
   - [评估脚本](#33-评估脚本)
   - [预处理脚本](#34-预处理脚本)

---

## 1. start_all_with_monitor.sh 详细解析

### 1.1 文件功能概述

`start_all_with_monitor.sh` 是KernelGYM的**单节点一体化启动脚本**，用于在单个节点上同时启动所有必要的服务组件。

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                    start_all_with_monitor.sh 功能架构                        │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │                        启动流程                                        │  │
│  │                                                                        │  │
│  │   1. 检查/生成.env配置                                                 │  │
│  │          ↓                                                             │  │
│  │   2. 检查/启动Redis服务                                                │  │
│  │          ↓                                                             │  │
│  │   3. 启动API Server                                                    │  │
│  │          ↓                                                             │  │
│  │   4. 启动Worker Monitor                                                │  │
│  │          ↓                                                             │  │
│  │   5. 启动所有GPU Workers                                               │  │
│  │          ↓                                                             │  │
│  │   6. 注册Workers到Redis                                                │  │
│  │                                                                        │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
│                                                                              │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 1.2 核心逻辑解析

#### 1.2.1 环境配置阶段

```bash
# 第8-11行: 路径初始化
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
AUTO_CONFIGURE="${ROOT_DIR}/scripts/auto_configure.sh"
ENV_FILE="${ROOT_DIR}/.env"

# 功能: 确定脚本所在目录，设置配置文件路径
```

#### 1.2.2 参数解析

```bash
# 第18-41行: 命令行参数解析
while [[ $# -gt 0 ]]; do
    case "$1" in
        --log-dir)          # 自定义日志目录
            LOG_DIR_OVERRIDE="$2"
            shift 2
            ;;
        --use-indexed-ports) # 使用索引端口 (PORT0, PORT1...)
            AUTO_CONFIGURE_ARGS+=("--use-indexed-ports")
            shift 1
            ;;
        --force-config)      # 强制重新生成配置
            AUTO_CONFIGURE_ARGS+=("--force")
            shift 1
            ;;
    esac
done
```

**参数说明:**

| 参数 | 说明 | 示例 |
|------|------|------|
| `--log-dir` | 指定日志输出目录 | `--log-dir /var/log/kernelgym` |
| `--use-indexed-ports` | 使用PORT0, PORT1...环境变量作为端口候选 | `--use-indexed-ports` |
| `--force-config` | 强制重新生成.env配置文件 | `--force-config` |

#### 1.2.3 自动配置检查

```bash
# 第43-55行: 检查并生成配置
if [ ! -f "${ENV_FILE}" ]; then
    echo "No .env found. Running auto configuration..."
    "${AUTO_CONFIGURE}" "${AUTO_CONFIGURE_ARGS[@]}"
elif [ ${#AUTO_CONFIGURE_ARGS[@]} -gt 0 ]; then
    echo "Re-running auto configuration with explicit flags..."
    "${AUTO_CONFIGURE}" "${AUTO_CONFIGURE_ARGS[@]}"
fi
```

#### 1.2.4 Redis服务检查与启动

```bash
# 第78-107行: Redis检查和启动逻辑
port_is_open() {
    # 使用Python socket检查端口是否开放
    python - "$host" "$port" <<PY
import socket, sys
try:
    with socket.create_connection((host, port), timeout=1):
        pass
    sys.exit(0)
except Exception:
    sys.exit(1)
PY
}

# 如果Redis不可达且是本地配置，自动启动Redis
if ! port_is_open "${REDIS_HOST}" "${REDIS_PORT}"; then
    if [ "${REDIS_HOST}" != "localhost" ] && [ "${REDIS_HOST}" != "127.0.0.1" ]; then
        echo "Redis is not reachable at ${REDIS_HOST}:${REDIS_PORT}. Please start it first."
        exit 1
    fi
    # 启动本地Redis
    redis-server --port "${REDIS_PORT}" --daemonize yes
fi
```

#### 1.2.5 服务启动

```bash
# 第109-117行: 启动API Server和Worker Monitor
echo "Starting API server..."
python -m kernelgym.server.api.server > "${ROOT_DIR}/${LOG_DIR}/api_server.log" 2>&1 &
API_PID=$!

echo "Starting worker monitor..."
python -m kernelgym.worker.worker_monitor --persistent > "${ROOT_DIR}/${LOG_DIR}/worker_monitor.log" 2>&1 &
MONITOR_PID=$!
```

#### 1.2.6 GPU Worker启动

```bash
# 第121-176行: 解析GPU列表并启动Workers
GPU_LIST="$(python - <<'PY'
import os, json
raw = os.environ.get("GPU_DEVICES", "")
# 解析JSON格式的GPU列表，如 [0,1,2,3]
parsed = json.loads(raw)
print(" ".join(str(x) for x in parsed))
PY
)"

# 为每个GPU启动一个Worker
for gpu in ${GPU_LIST}; do
    WORKER_ID="worker_gpu_${gpu}"
    python -m kernelgym.worker.single_worker \
        --worker-id "${WORKER_ID}" \
        --device "cuda:${gpu}" \
        --persistent \
        > "${ROOT_DIR}/${LOG_DIR}/worker_gpu_${gpu}.log" 2>&1 &
    
    # 注册Worker到Redis
    redis-cli SADD "${REDIS_KEY_PREFIX}:expected_workers" "${WORKER_ID}"
done
```

### 1.3 使用方法

```bash
# 基本使用
./start_all_with_monitor.sh

# 指定日志目录
./start_all_with_monitor.sh --log-dir /path/to/logs

# 强制重新生成配置
./start_all_with_monitor.sh --force-config

# 使用索引端口
./start_all_with_monitor.sh --use-indexed-ports

# 组合使用
./start_all_with_monitor.sh --log-dir ./my_logs --force-config
```

### 1.4 注意事项

| 注意事项 | 说明 |
|----------|------|
| **环境变量** | 确保`.env`文件中`GPU_DEVICES`正确配置，如`[0,1,2,3]` |
| **端口冲突** | 如果端口被占用，脚本会自动选择下一个可用端口 |
| **Redis依赖** | 本地模式会自动启动Redis，远程模式需要预先启动 |
| **日志管理** | 日志文件会持续增长，建议定期清理 |
| **进程管理** | 脚本启动的是后台进程，需要手动停止或使用系统服务管理 |

---

## 2. start_worker_node.sh 详细解析

### 2.1 文件功能概述

`start_worker_node.sh` 是KernelGYM的**工作节点启动脚本**，用于在分布式部署中启动远程Worker节点，连接到主节点的API和Redis服务。

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                    start_worker_node.sh 功能架构                             │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │                        启动流程                                        │  │
│  │                                                                        │  │
│  │   1. 加载server.env配置                                               │  │
│  │          ↓                                                             │  │
│  │   2. 连接性检查 (Redis + API)                                         │  │
│  │          ↓                                                             │  │
│  │   3. 节点注册/分配NODE_ID                                             │  │
│  │          ↓                                                             │  │
│  │   4. 清理旧Worker进程                                                 │  │
│  │          ↓                                                             │  │
│  │   5. 启动WorkerManager                                                │  │
│  │          ↓                                                             │  │
│  │   6. 等待Workers注册完成                                              │  │
│  │                                                                        │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
│                                                                              │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 2.2 核心逻辑解析

#### 2.2.1 配置加载

```bash
# 第14-34行: 配置文件加载
SERVER_ENV_PATH="${1:-${ROOT_DIR}/server.env}"

if [[ ! -f "$SERVER_ENV_PATH" ]]; then
  echo "server.env not found: $SERVER_ENV_PATH"
  exit 1
fi

# 如果本地.env不存在，从server.env复制
if [[ ! -f .env ]]; then
  cp "$SERVER_ENV_PATH" .env
fi

# 加载环境变量
set -o allexport
source .env
set +o allexport
```

#### 2.2.2 连接性预检查

```bash
# 第44-70行: Redis和API连接检查
# Redis检查
if redis-cli -h "${REDIS_HOST}" -p "${REDIS_PORT}" PING >/dev/null 2>&1; then
  echo "Redis OK"
else
  echo "FAILED - Cannot connect to Redis"
  exit 1
fi

# API检查
if curl -s --max-time 5 "${API_URL_CHECK}/health" >/dev/null 2>&1; then
  echo "API OK"
else
  echo "FAILED - Cannot reach API server"
  exit 1
fi
```

#### 2.2.3 节点注册机制

```bash
# 第77-151行: 节点ID分配逻辑
if [[ -z "${NODE_ID:-}" ]]; then
  # 没有预设NODE_ID，请求服务器分配
  RESP=$(curl -sS -X POST "${API_BASE}/node/allocate?hostname=${HOSTNAME_VAL}")
  NODE_ID=$(echo "${RESP}" | python3 -c 'import sys,json; print(json.load(sys.stdin).get("node_id",""))')
else
  # 使用预设NODE_ID，向服务器注册
  curl -sS -X POST "${API_BASE}/node/allocate?hostname=${HOSTNAME_VAL}&node_name=${NODE_ID}"
fi
```

#### 2.2.4 Worker启动

```bash
# 第158-171行: 启动WorkerManager
# 清理旧进程
pkill -f "python.*kernelgym.worker.gpu_worker" 2>/dev/null || true

# 启动WorkerManager
nohup python3 -m kernelgym.worker.gpu_worker > logs/worker_manager.log 2>&1 &
WORKER_MGR_PID=$!

# 信号处理
cleanup() {
  kill -TERM "${WORKER_MGR_PID}"
  pkill -TERM -P "${WORKER_MGR_PID}"
}
trap cleanup SIGINT SIGTERM
```

### 2.3 使用方法

```bash
# 基本使用 (使用默认server.env)
./start_worker_node.sh

# 指定配置文件
./start_worker_node.sh /path/to/server.env

# 前置条件: 需要server.env文件
cat > server.env << EOF
API_HOST=192.168.1.100
API_PORT=10907
REDIS_HOST=192.168.1.100
REDIS_PORT=6379
GPU_DEVICES=[0,1,2,3]
NODE_ID=worker-node-1
EOF
```

### 2.4 与start_all_with_monitor.sh的区别

| 特性 | start_all_with_monitor.sh | start_worker_node.sh |
|------|---------------------------|----------------------|
| **部署模式** | 单节点一体化 | 分布式Worker节点 |
| **启动API** | ✅ 是 | ❌ 否 |
| **启动Redis** | ✅ 是（本地） | ❌ 否（连接远程） |
| **节点注册** | ❌ 不需要 | ✅ 需要 |
| **配置来源** | 自动生成.env | 从server.env复制 |
| **适用场景** | 开发/测试/单机部署 | 生产环境分布式部署 |

---

## 3. 训练脚本目录详细解析

### 3.1 目录结构总览

```
drkernel/kernel/scripts/
├── rl/                          # RL训练脚本
│   ├── 8b_trloo_mrs_pr_prs.sh   # 8B模型RL训练
│   ├── 14b_trloo_mrs_pr_prs.sh  # 14B模型RL训练
│   └── train_rl_common.sh       # RL训练公共逻辑
├── sft/                         # SFT训练脚本
│   ├── 8b-coldstart.sh          # 8B模型SFT训练
│   └── 14b-coldstart.sh         # 14B模型SFT训练
├── eval/                        # 评估脚本
│   ├── grading_common.sh        # 评估公共逻辑
│   ├── drkernel-14b-maxturns3.sh    # Dr.Kernel 14B评估
│   ├── drkernel-14b-maxturns5-maxiter10.sh  # 多轮迭代评估
│   ├── claude-4.5-sonnet-level2.sh  # Claude评估
│   └── claude-4.5-sonnet-level2-compile.sh  # Claude编译评估
└── preprocess/                  # 预处理脚本
    ├── pull_from_hub.py         # 从HuggingFace拉取数据
    ├── push_to_hub.py           # 推送数据到HuggingFace
    └── push.sh                  # 推送脚本
```

---

### 3.2 RL训练脚本

#### 3.2.1 train_rl_common.sh - RL训练公共逻辑

**文件功能:** 提供RL训练的通用配置和执行逻辑，所有具体训练脚本都需要source此文件。

**核心功能模块:**

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                    train_rl_common.sh 模块架构                               │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │ 配置模块                                                               │  │
│  │ ├── 数据集配置 (TRAIN_DATASET, VALID_DATASET)                         │  │
│  │ ├── 模型配置 (MODEL_NAME, MODEL_PATH)                                 │  │
│  │ ├── 奖励配置 (REWARD_MANAGER, COVERAGE_REWARD_*)                      │  │
│  │ ├── 算法配置 (ALGORITHM, CLIP_RATIO, LEARNING_RATE)                   │  │
│  │ └── 系统配置 (NNODES, GPUS_PER_NODE)                                  │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
│                                                                              │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │ 函数模块                                                               │  │
│  │ ├── generate_model_micro_token()  # 根据模型大小计算micro token数     │  │
│  │ ├── generate_suffix()             # 生成实验名称后缀                  │  │
│  │ ├── parse_arguments()             # 解析命令行参数                    │  │
│  │ ├── setup_training_environment()  # 设置训练环境                      │  │
│  │ └── run_training()                # 执行训练                          │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
│                                                                              │
└─────────────────────────────────────────────────────────────────────────────┘
```

**关键配置参数:**

| 参数类别 | 参数名 | 默认值 | 说明 |
|----------|--------|--------|------|
| **算法** | `ALGORITHM` | grpo | 可选: grpo, trloo |
| **学习率** | `LEARNING_RATE` | 1e-6 | 训练学习率 |
| **批次大小** | `TRAIN_BATCH_SIZE` | 512 | 训练批次大小 |
| **PPO配置** | `PPO_MINI_BATCH_SIZE` | 32 | PPO mini批次大小 |
| **裁剪比率** | `CLIP_RATIO` | 0.2_0.28 | Dual-clip PPO裁剪范围 |
| **多轮配置** | `MAX_TURN` | 3 | 最大对话轮次 |
| **覆盖率奖励** | `COVERAGE_REWARD_WEIGHT` | 0.25 | 覆盖率奖励权重 |
| **拒绝采样** | `ROLLOUT_RS` | null | 拒绝采样策略 |

**输入输出要求:**

```
输入:
├── 训练数据集 (HuggingFace路径或本地parquet文件)
├── 验证数据集 (可选)
├── 预训练模型路径
└── KernelGYM服务URL

输出:
├── 模型检查点 (checkpoints/RUN_NAME/)
├── 训练日志 (logs/RUN_NAME.log)
└── WandB实验记录
```

**应用场景:**
- 作为所有RL训练脚本的基类
- 提供统一的训练流程和参数解析
- 支持命令行参数覆盖默认配置

---

#### 3.2.2 8b_trloo_mrs_pr_prs.sh - 8B模型RL训练

**文件功能:** 8B参数模型的TRLOO算法训练脚本，集成MRS、PR、PRS技术。

**关键配置:**

```bash
# 模型配置
MODEL_NAME=hkust-nlp/drkernel-8b
ALGORITHM="trloo"

# 奖励配置
REWARD_FUNC_NAME="calculate_reward_speedup"
COVERAGE_REWARD_TYPE="time_coverage"
COVERAGE_REWARD_WEIGHT=0.5
COVERAGE_REWARD_ENABLE=True

# 拒绝采样配置
ROLLOUT_RS="geometric"           # 几何拒绝采样
COVERAGE_RS="turn"               # 基于turn的覆盖率拒绝采样
COVERAGE_RS_THRESHOLD=0.3        # 覆盖率阈值

# 多轮配置
ENABLE_MULTI_TURN=True
MAX_TURN=3
```

**技术特性:**

| 技术 | 配置 | 作用 |
|------|------|------|
| **TRLOO** | `ALGORITHM=trloo` | Turn-level REINFORCE Leave-One-Out |
| **MRS** | `ROLLOUT_RS=geometric` | Multi-turn Rejection Sampling |
| **PR** | `COVERAGE_REWARD_ENABLE=True` | Profiling-based Rewards |
| **PRS** | `COVERAGE_RS=turn` | Profiling-based Rejection Sampling |

**使用方法:**

```bash
# 基本使用
bash 8b_trloo_mrs_pr_prs.sh

# 自定义参数
bash 8b_trloo_mrs_pr_prs.sh \
    --learning_rate 5e-7 \
    --train_batch_size 8 \
    --max_turn 2

# 设置KernelGYM服务
export KERNELGYM_SERVER_URL="http://localhost:10907"
bash 8b_trloo_mrs_pr_prs.sh
```

---

#### 3.2.3 14b_trloo_mrs_pr_prs.sh - 14B模型RL训练

**文件功能:** 14B参数模型的TRLOO算法训练脚本，配置与8B版本类似，但针对更大模型进行了优化。

**与8B版本的主要差异:**

```bash
# 模型配置
MODEL_NAME=hkust-nlp/drkernel-14b

# 其他配置基本相同，但可能调整:
# - SP_SIZE (序列并行大小)
# - PPO_MICRO_TOKEN (根据模型大小自动计算)
```

---

### 3.3 SFT训练脚本

#### 3.3.1 8b-coldstart.sh - 8B模型SFT冷启动训练

**文件功能:** 8B模型的监督微调(SFT)冷启动训练脚本，用于在RL训练前初始化模型。

**核心配置:**

```bash
# 数据配置
DATASET_NAME=hkust-nlp/drkernel-coldstart-8k
TRAIN_FILE_NAME=train_2000

# 训练配置
TRAIN_BATCH_SIZE=64
MICRO_BATCH_SIZE_PER_GPU=2
MAX_LENGTH=18432
TOTAL_EPOCHS=4
LEARNING_RATE=2e-5

# 模型配置
MODEL_NAME=qwen3-8b-base
SP_SIZE=4  # Ulysses序列并行大小
```

**训练流程:**

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                        SFT训练流程                                           │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│  1. 加载预训练模型 (Qwen3-8B-Base)                                          │
│          ↓                                                                   │
│  2. 加载SFT数据集 (drkernel-coldstart-8k)                                   │
│          ↓                                                                   │
│  3. 配置FSDP训练                                                            │
│     - enable_gradient_checkpointing=True                                    │
│     - cpu_offload=True                                                      │
│     - ulysses_sequence_parallel_size=4                                      │
│          ↓                                                                   │
│  4. 执行训练 (4 epochs)                                                     │
│          ↓                                                                   │
│  5. 保存检查点                                                              │
│                                                                              │
└─────────────────────────────────────────────────────────────────────────────┘
```

**输入输出:**

```
输入:
├── 预训练模型: Qwen/Qwen2.5-7B (或指定路径)
├── 训练数据: hkust-nlp/drkernel-coldstart-8k
└── 格式: {prompt: str, response: str}

输出:
├── 检查点: checkpoints/drkernel-8b-coldstart/
├── 日志: logs/drkernel-8b-coldstart.log
└── WandB记录
```

**使用方法:**

```bash
# 基本使用
bash 8b-coldstart.sh

# 自定义参数
bash 8b-coldstart.sh \
    --train_batch_size 32 \
    --learning_rate 1e-5 \
    --total_epochs 2

# 多节点训练
export NNODES=2
export GPUS_PER_NODE=8
export MASTER_ADDR=192.168.1.100
bash 8b-coldstart.sh
```

---

#### 3.3.2 14b-coldstart.sh - 14B模型SFT训练

**文件功能:** 14B模型的SFT训练脚本，配置与8B版本类似。

**主要差异:**

```bash
MODEL_NAME=qwen3-14b-base
RUN_NAME=drkernel-14b-coldstart
```

---

### 3.4 评估脚本

#### 3.4.1 grading_common.sh - 评估公共逻辑

**文件功能:** 提供内核代码评估的通用配置和执行逻辑，所有评估脚本都需要source此文件。

**核心功能模块:**

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                    grading_common.sh 模块架构                                │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │ 配置模块                                                               │  │
│  │ ├── 数据配置 (EVAL_DATASET, OUTPUT_PATH)                              │  │
│  │ ├── 模型配置 (MODEL_NAME, MODEL_PATH)                                 │  │
│  │ ├── 生成配置 (N_SAMPLES, TEMPERATURE, TOP_P)                          │  │
│  │ ├── 评估配置 (SOLVE_THRESHOLD, PASS_AT_K)                             │  │
│  │ ├── 奖励配置 (REWARD_MANAGER, REWARD_WEIGHTS)                         │  │
│  │ └── Rollout配置 (ROLLOUT_MODE, ROLLOUT_GPU_MEMORY_UTIL)               │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
│                                                                              │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │ 函数模块                                                               │  │
│  │ ├── parse_arguments()          # 解析命令行参数                       │  │
│  │ ├── setup_grading_environment() # 设置评估环境                        │  │
│  │ ├── parse_reward_weights()     # 解析奖励权重                        │  │
│  │ └── run_grading()              # 执行评估                             │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
│                                                                              │
└─────────────────────────────────────────────────────────────────────────────┘
```

**关键配置参数:**

| 参数类别 | 参数名 | 默认值 | 说明 |
|----------|--------|--------|------|
| **生成** | `N_SAMPLES` | 4 | 每个prompt生成的样本数 |
| | `BATCH_SIZE` | 8 | 批次大小 |
| | `TEMPERATURE` | 0.8 | 采样温度 |
| | `TOP_P` | 0.95 | Top-p采样 |
| **评估** | `SOLVE_THRESHOLD` | 0.99 | 解决阈值 |
| | `PASS_AT_K` | 1 | Pass@k值 |
| **奖励** | `REWARD_WEIGHTS` | 0.3_0.4_0.3 | 编译/正确/性能权重 |
| | `NUM_PERF_TRIALS` | 100 | 性能测试次数 |
| **Rollout** | `ROLLOUT_MODE` | sync | sync/async_vllm/async_agent |

**输入输出:**

```
输入:
├── 评估数据集 (parquet格式)
├── 模型检查点路径
└── KernelGYM服务URL (可选)

输出:
├── 评估结果 (parquet格式，包含solve_rate列)
├── 原始响应 (可选，JSONL格式)
├── 指标统计 (可选，JSON格式)
└── DataProto缓存 (可选)
```

---

#### 3.4.2 drkernel-14b-maxturns3.sh - Dr.Kernel 14B评估

**文件功能:** Dr.Kernel 14B模型的多轮评估脚本，支持最多3轮对话。

**关键配置:**

```bash
# 模型配置
MODEL_NAME=hkust-nlp/drkernel-14b
MODEL_PATH=${MODEL_NAME}

# 多轮配置
MULTI_TURN=True
MAX_USER_TURNS=3

# 生成配置
N_SAMPLES=8
BATCH_SIZE=128
TEMPERATURE=1.0
TOP_P=0.95

# Rollout配置
ROLLOUT_MODE="async_vllm"
ROLLOUT_GPU_MEMORY_UTIL=0.5

# 奖励配置
REWARD_MANAGER="kernel_async"
REWARD_FUNC_NAME="calculate_reward_speedup"
REWARD_WEIGHTS="0.3_0.4_0.3"
```

**评估流程:**

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                        评估流程                                              │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│  1. 加载评估数据集                                                          │
│          ↓                                                                   │
│  2. 初始化模型和vLLM引擎                                                    │
│          ↓                                                                   │
│  3. 多轮生成                                                                │
│     for turn in range(MAX_USER_TURNS):                                      │
│         response = model.generate(prompt)                                   │
│         prompt = update_prompt(response)                                    │
│          ↓                                                                   │
│  4. 奖励计算 (通过KernelGYM)                                               │
│     - 编译检查                                                              │
│     - 正确性验证                                                            │
│     - 性能测量                                                              │
│          ↓                                                                   │
│  5. 计算指标                                                                │
│     - solve_rate                                                            │
│     - pass@k                                                                │
│     - average_speedup                                                       │
│          ↓                                                                   │
│  6. 保存结果                                                                │
│                                                                              │
└─────────────────────────────────────────────────────────────────────────────┘
```

**使用方法:**

```bash
# 设置KernelGYM服务
export KERNELGYM_SERVER_URL="http://localhost:10907"

# 基本使用
bash drkernel-14b-maxturns3.sh

# 自定义参数
bash drkernel-14b-maxturns3.sh \
    --n_samples 16 \
    --temperature 0.7 \
    --eval_dataset my_data.parquet \
    --output_path results.parquet
```

---

#### 3.4.3 claude-4.5-sonnet-level2.sh - Claude模型评估

**文件功能:** 使用Claude 4.5 Sonnet模型进行内核代码生成的评估脚本。

**关键配置:**

```bash
# 使用OpenAI兼容接口
BACKEND="openai"
OPENAI_MODEL="claude-4.5-sonnet"
OPENAI_API_KEY="${ANTHROPIC_API_KEY}"
OPENAI_BASE_URL="https://api.anthropic.com"
```

---

### 3.5 预处理脚本

#### 3.5.1 pull_from_hub.py - 从HuggingFace拉取数据

**文件功能:** 从HuggingFace Hub下载数据集。

**使用方法:**

```bash
python pull_from_hub.py \
    --repo_id hkust-nlp/drkernel-rl-data \
    --local_dir ./data/drkernel-rl-data
```

#### 3.5.2 push_to_hub.py - 推送数据到HuggingFace

**文件功能:** 将本地数据推送到HuggingFace Hub。

**使用方法:**

```bash
python push_to_hub.py \
    --repo_id my-username/my-dataset \
    --local_dir ./data/my-data \
    --commit_message "Add new data"
```

---

## 4. 脚本使用流程总结

### 4.1 完整训练流程

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                        完整训练流程                                          │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│  Step 1: 启动KernelGYM服务                                                  │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │ ./start_all_with_monitor.sh                                          │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│          ↓                                                                   │
│  Step 2: SFT冷启动训练                                                      │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │ cd drkernel/kernel/scripts/sft                                       │   │
│  │ bash 8b-coldstart.sh                                                 │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│          ↓                                                                   │
│  Step 3: RL训练                                                             │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │ cd drkernel/kernel/scripts/rl                                        │   │
│  │ export KERNELGYM_SERVER_URL="http://localhost:10907"                 │   │
│  │ bash 8b_trloo_mrs_pr_prs.sh                                          │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│          ↓                                                                   │
│  Step 4: 模型评估                                                           │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │ cd drkernel/kernel/scripts/eval                                      │   │
│  │ bash drkernel-14b-maxturns3.sh                                       │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│                                                                              │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 4.2 分布式部署流程

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                        分布式部署流程                                        │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│  主节点:                                                                     │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │ 1. redis-server --bind 0.0.0.0                                       │   │
│  │ 2. python -m kernelgym.server.api.server                            │   │
│  │ 3. 创建server.env文件                                                │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│                                                                              │
│  Worker节点:                                                                 │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │ 1. 复制server.env到本地                                              │   │
│  │ 2. ./start_worker_node.sh server.env                                 │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│                                                                              │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 5. 常见问题与解决方案

### 5.1 启动脚本问题

| 问题 | 原因 | 解决方案 |
|------|------|----------|
| `.env not found` | 配置文件不存在 | 运行`bash scripts/auto_configure.sh` |
| `Redis not reachable` | Redis未启动 | 检查Redis服务或使用本地模式 |
| `Port already in use` | 端口被占用 | 使用`--use-indexed-ports`或手动修改端口 |
| `GPU_DEVICES parse error` | 格式错误 | 确保格式为`[0,1,2,3]` |

### 5.2 训练脚本问题

| 问题 | 原因 | 解决方案 |
|------|------|----------|
| `CUDA out of memory` | 显存不足 | 降低`TRAIN_BATCH_SIZE`或启用offload |
| `vLLM init failed` | vLLM版本不兼容 | 确保使用vLLM 0.10.2 |
| `Reward timeout` | KernelGYM服务不可达 | 检查`KERNELGYM_SERVER_URL` |
| `NaN loss` | 训练不稳定 | 降低`LEARNING_RATE` |

### 5.3 评估脚本问题

| 问题 | 原因 | 解决方案 |
|------|------|----------|
| `Model not found` | 模型路径错误 | 检查`MODEL_PATH`配置 |
| `Dataset not found` | 数据集路径错误 | 检查`EVAL_DATASET`配置 |
| `Rollout failed` | vLLM配置问题 | 检查`ROLLOUT_GPU_MEMORY_UTIL` |
