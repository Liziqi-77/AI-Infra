# 从 SFT 到 KernelGYM 的 RL：TRLOO 多轮训练端到端学习指南

> **面向读者**：已经理解 SFT（监督微调），但刚开始学习 RL（强化学习）的工程师。  
> **源码依据**：本文依据当前本地 checkout 的实现撰写；主要入口是 `drkernel/kernel/scripts/rl/8b_trloo_mrs_pr_prs.sh`。行号是写作时的当前版本，后续提交可能改变它们。  
> **目标**：读完后，你应能解释一条 RL 命令如何把 prompt 变成多个多轮轨迹、如何经 KernelGYM 获得编译/正确性/性能反馈、怎样得到 TRLOO advantage，以及 PPO 最终如何更新模型。

---

## 目录

1. [先建立全局心智模型：SFT 与本项目 RL 的差异](#1-先建立全局心智模型sft-与本项目-rl-的差异)
2. [术语、对象与总数据流](#2-术语对象与总数据流)
3. [环境依赖：服务端与训练端是两套系统](#3-环境依赖服务端与训练端是两套系统)
4. [从零拉起一次 RL：每步在做什么](#4-从零拉起一次-rl每步在做什么)
5. [RL 启动脚本逐行解读：`8b_trloo_mrs_pr_prs.sh`](#5-rl-启动脚本逐行解读8b_trloo_mrs_pr_prssh)
6. [公共启动器与 Hydra 命令逐行解读](#6-公共启动器与-hydra-命令逐行解读)
7. [命令启动后：配置、Ray 与训练器如何建立](#7-命令启动后配置ray-与训练器如何建立)
8. [Rollout：模型怎样完成三轮“生成—反馈—改进”](#8-rollout模型怎样完成三轮生成反馈改进)
9. [Reward：KernelGYM 如何把代码变成数值奖励](#9-rewardkernelgym-如何把代码变成数值奖励)
10. [TRLOO：为什么需要 advantage，项目如何计算它](#10-trloo为什么需要-advantage项目如何计算它)
11. [PPO：advantage 如何真正改动模型参数](#11-ppoadvantage-如何真正改动模型参数)
12. [MRS、PR、PRS、筛选与采样：本配方到底启用了什么](#12-mrsprprs筛选与采样本配方到底启用了什么)
13. [训练观察、缩小实验与排错](#13-训练观察缩小实验与排错)
14. [一轮训练的完整调用链速查](#14-一轮训练的完整调用链速查)

---

## 1. 先建立全局心智模型：SFT 与本项目 RL 的差异

### 1.1 你已经熟悉的 SFT

一条 SFT 样本通常是 `(prompt, 标准答案)`。模型在 teacher forcing 下看到标准答案前缀，最小化交叉熵：

$$
\mathcal L_{\text{SFT}} = -\sum_t \log \pi_\theta(y_t^* \mid x, y^*_{<t})
$$

其中：

- `x` 是 prompt；
- `y*` 是人或数据集给出的标准 token 序列；
- $\pi_\theta$ 是参数为 $\theta$ 的语言模型；
- 每个 token 的“正确目标”在训练开始前已经写在数据中。

**SFT 的本质**：让模型模仿已给定答案分布。

### 1.2 KernelGYM 中的 RL

Kernel 代码优化很难为每个 prompt 准备唯一的“最佳实现”。两个都正确的 Triton kernel，可能一个更快；甚至模型能通过多次尝试、读取编译或 profiling 反馈后修正代码。因此这里使用结果型（outcome）奖励：

$$
J(\theta)=\mathbb E_{y\sim\pi_\theta(\cdot\mid x)}[R(x,y)]
$$

训练不是指定模型必须输出什么 token，而是：

1. 让当前模型对一个优化任务生成多个候选实现；
2. KernelGYM 服务端编译、运行、校验并测速；
3. 将结果折算为 reward；
4. 提高“比同题其他候选更好”的轨迹的概率，降低更差轨迹的概率。

**RL 的本质**：从环境打分中学习能带来更高期望回报的输出分布。

> 项目 README 推荐先做 SFT cold start（`drkernel/README.md:109–128`）。这不是框架强制条件，而是实践上非常重要：若模型连合法的代码格式、工具调用/反馈格式都没学到，RL 得到的大量是编译失败的低奖励，学习信号会很弱。

### 1.3 为什么不是“直接拿 reward 乘 loss”

直接最大化 reward 的梯度估计会有很大方差。对同一 prompt，偶然采到一个好结果并不一定表示策略真的更好。KernelGYM 的主配方因此采用：

- **多样本 rollout**：每个 prompt 采样 `ROLLOUT_N=16` 条候选；
- **多轮交互**：每个候选最多 `MAX_TURN=3` 轮；
- **TRLOO**：以同一题、同一轮的“其他候选平均回报”作 leave-one-out 基线；
- **PPO**：限制一次更新不能让策略偏离产生 rollout 的旧策略太远；
- **rejection / coverage 筛选**：丢弃信息量低或不符合规则的样本，并用过采样弥补数量。

---

## 2. 术语、对象与总数据流

### 2.1 关键术语

| 名称 | 本文含义 | 在代码中主要对应 |
|---|---|---|
| policy / actor | 要训练的 LLM；给定上下文输出下一个 token 的分布 | `actor_rollout_ref.actor` |
| rollout | 用当前/旧策略实际采样一条回答或完整交互轨迹 | vLLM async rollout worker |
| prompt | 一道 kernel 优化任务，含参考实现、入口等信息 | RL parquet 数据的一条原始样本 |
| trajectory | 一个 prompt 的一次候选经历；本项目中可含 3 个 turn | `uid` + `turn_indices` |
| environment | 对模型输出执行并反馈的外部世界 | KernelGYM API + GPU worker |
| reward | 环境对结果的标量评分 | `token_level_scores` 的末 token 位置 |
| return | 从一个 turn 到轨迹结尾累积的奖励 | `returns` |
| baseline | 用来减少方差的比较基准，不是额外奖励 | 同题同轮的其他 rollout 平均 return |
| advantage | “比基线好多少”：$A=G-b$ | `advantages` |
| old policy | 产生当前 batch rollout 时的策略 | `old_log_probs` |
| reference policy | 用于 KL 约束的冻结参考模型；本配方默认不启用 | `ref_log_prob` |
| FSDP | Fully Sharded Data Parallel；分片存模型、梯度、优化器状态 | `verl_patch.workers.code.fsdp_workers` |

### 2.2 一张总图

```text
RL parquet 数据
  prompt + ground_truth/reference_code + entry_point + uuid
             │
             ▼
RayKernelTrainer.fit()
  每 prompt 采 ROLLOUT_N=16 条、多轮最多 MAX_TURN=3
             │
             ▼
async vLLM + KernelAgent
  第 1 轮生成 kernel code
  第 2/3 轮读取 KernelGYM 的反馈并改进
             │
             ▼
AsyncKernelRewardManager
  extract_kernel_code → KernelRewardClient
             │ HTTP POST /evaluate, GET /status/{id}, GET /results/{id}
             ▼
KernelGYM API + Redis + GPU worker
  编译 → correctness trials → performance trials → profiling
             │
             ▼
reward（终止 token 上的 score）
  correctness / speedup / coverage / decoy 等附加信息
             │
             ▼
筛选、mask、TRLOO
  同 uid + 同 turn 的其余 rollout 形成 LOO baseline
             │
             ▼
PPO actor update
  log-prob ratio + dual clipping + backward + optimizer step
             │
             ▼
checkpoint、console/W&B metrics、下一轮 rollout
```

### 2.3 一个 batch 的数量关系

假设启动器当前默认值：

- `TRAIN_BATCH_SIZE=16`：一次输入 16 个 prompt；
- `ROLLOUT_N=16`：每个 prompt 采 16 个候选；
- `MAX_TURN=3`：每候选最多 3 轮。

在没有超时、空 turn 或筛选的理想上界，生成/训练记录数是：

$$
16\text{ prompts}\times16\text{ rollouts/prompt}\times3\text{ turns}=768\text{ turn records}
$$

它**不是** 768 个独立问题：每 16 个候选共享一个原始 prompt；每 3 条 turn record 又属于同一候选轨迹。`kernel_trainer.py:2900–2912` 正是按照 `ROLLOUT_N × MAX_TURN` 复制原 batch，使它能与生成输出对齐。

---

## 3. 环境依赖：服务端与训练端是两套系统

不要把 KernelGYM 当成“只需一条 `torchrun` 的训练脚本”。它由评测服务和 RL 训练服务协作；可同机，也可不同机。

### 3.1 层 A：KernelGYM 评测服务

这是环境。它接收候选 kernel，调度 GPU worker 做编译、正确性检查、性能测量和 profiling。

| 依赖/资源 | 是否必要 | 原因与源码依据 |
|---|---:|---|
| Linux（README 推荐 Ubuntu 20.04+） | 是 | 根 `README.md:135–140` |
| Python 3.10+ | 是 | 根 `README.md:135–140` |
| CUDA 11.8+ 与兼容 GPU | 是 | kernel 执行、评测 GPU worker；根 `README.md:135–140` |
| Redis | 是 | 任务队列、worker 监控/状态；`requirements.txt:27–30`、`start_all_with_monitor.sh:94–107` |
| FastAPI/Uvicorn、Celery 等 Python 包 | 是 | API 与服务基础依赖；根 `requirements.txt` |
| `iproute2` | 服务启动脚本会安装 | 根 `setup.sh:6–9` |
| API server + worker monitor + 至少一个 GPU worker | 是 | `start_all_with_monitor.sh:109–176` |
| Docker sandbox 镜像 | 取决于 `.env`/服务配置 | 不要假定仓库自动为你构建镜像；按当前部署配置确认 |

根目录安装命令：

```bash
cd /home/l00899543/RL/KernelGYM
bash setup.sh
```

它做了三件事（`setup.sh:6–9`）：用 `pip --user` 安装根 `requirements.txt`、安装 `pydantic-settings`、通过 `sudo apt` 安装 `iproute2` 和 Redis。因为会使用 `sudo` 并改动机器环境，请在自己的训练机器/虚拟环境策略下执行。

### 3.2 层 B：Dr.Kernel / VERL RL 训练端

训练端位于 `KernelGYM/drkernel`，依赖 VERL、Ray、vLLM、FSDP 和高版本 PyTorch/CUDA 组合。

| 依赖 | 当前脚本中的版本/行为 | 为什么需要 |
|---|---|---|
| VERL git submodule | `git submodule update --init`，随后 editable install | 提供 PPO/Ray/FSDP 基础设施 |
| Ray | `ray==2.47.1` | 调度 trainer、actor、rollout worker、HTTP reward worker |
| vLLM | `vllm==0.10.2` | 高吞吐生成与 async rollout |
| PyTorch family | `torch==2.8.0`、`torchvision==0.23.0`、`torchaudio==2.8.0` | actor 前向、反向、FSDP |
| Transformers | `transformers[hf_xet]==4.56.0` | 加载 Hugging Face 模型/tokenizer |
| FlashAttention | 脚本下载 `flash-attn 2.8.3` 的 CPython 3.10 / CUDA 12 / Torch 2.8 wheel | 长上下文生成和训练性能 |
| `datasets` / `pyarrow` / `pandas` | 安装脚本安装 | 读取 parquet 数据 |
| W&B | `wandb==0.16.6` | 可选实验上报；脚本仍会把 logger 配成 `['console','wandb']` |

安装：

```bash
cd /home/l00899543/RL/KernelGYM/drkernel
bash setup.sh
```

> **版本兼容性很重要**：`drkernel/setup.sh:13–34` 明确把 Ray、vLLM、Torch、Transformers 和 FlashAttention 绑定到一组组合。不要在同一个 Python 环境里先随意安装另一版 Torch/vLLM，再期待这套训练稳定工作。推荐为该项目建立独立 Python 环境，并确保该 FlashAttention wheel 的 Python ABI、CUDA、Torch 版本与你的环境一致。

### 3.3 GPU 与分布式资源

训练 launcher 的默认目标是 **每节点 8 张训练 GPU**：

- 任务脚本 `8b_trloo_mrs_pr_prs.sh:84–88` 从 `ARNOLD_WORKER_*` 读取平台变量；否则 `GPUS_PER_NODE=8`；
- 公共脚本最终把它解析为 `trainer.n_gpus_per_node`，同时使用 `NNODES`；
- `main_kernel.py:181–188` 根据 `nnodes × n_gpus_per_node` 建立 Ray 的全局资源池。

评测也要 GPU。若评测服务和训练在同一台机器上，不要把同一张 GPU 同时交给 vLLM/FSDP 和 KernelGYM worker：

- 给服务端 `.env` 的 `GPU_DEVICES` 分配一组 GPU；
- 给训练 job 的 `CUDA_VISIBLE_DEVICES` 或集群调度器分配另一组 GPU；
- 若使用远程评测服务，训练端只需能访问其 HTTP URL。

### 3.4 模型、数据、输出与环境变量

| 项目 | 含义 | 当前脚本如何解析 |
|---|---|---|
| `KERNELGYM_SERVER_URL` | KernelGYM API 根地址 | launcher 读取环境变量；空值会导致奖励管理器报错 |
| `MODEL_PATH` | 显式模型本地路径或可加载的 Hugging Face 模型 ID | 若非空优先使用；见 `train_rl_common.sh:610–616` |
| `MODEL_NAME` | 用于默认模型 ID、run name、按 `8B/14B` 推导 token 微批上限 | **不必然等于实际模型路径** |
| `TRAIN_DATASET` / `VALID_DATASET` | RL 训练/验证 parquet 路径列表 | 相对名字会补成 `${HDFS_DATA_PATH}/<name>.parquet` |
| `HDFS_DATA_PATH` | 数据根目录；名字是历史遗留，也可为本地目录 | `setup_env.sh:18–21` 默认到 `drkernel/data` |
| `HDFS_CHECKPOINT_PATH` | checkpoint 根目录 | 默认 `drkernel/checkpoints`，最终附加 run name |
| `PROJECT_NAME` | W&B/console 项目名 | `setup_env.sh:14` 默认 `drkernel` |
| `WANDB_API_KEY` | W&B 登录凭据 | W&B 可用时需要；不要写进脚本或仓库 |

数据必须满足训练器需要的字段。就本教程关注的奖励调用而言，至少要能提供模型 prompt、`ground_truth`（参考实现）、`entry_point` 与 `uuid` 等任务元数据；实际 schema 请以训练 dataloader 和公开 parquet 数据为准。**仓库没有在 launcher 中自动把 Hugging Face dataset ID 下载并转换成 parquet**：`TRAIN_DATASET=("hkust-nlp/drkernel-rl-data")` 会在默认本地数据根下解析为 `.../hkust-nlp/drkernel-rl-data.parquet`。准备好本地 parquet 或传递绝对路径。

---

## 4. 从零拉起一次 RL：每步在做什么

下面给出学习/真实运行的顺序。不要跳过健康检查；RL 训练前发现奖励服务不可用，比数小时后得到全零 reward 好得多。

### 步骤 0：准备隔离环境和资源布局

确认：

```bash
python --version
nvidia-smi
```

目标是确认 Python、CUDA 驱动和可用 GPU。然后决定：

- KernelGYM 在本机还是远程机器；
- 哪些 GPU 给评测 worker，哪些给训练；
- 模型和 parquet 的绝对路径；
- checkpoint 是否写入有足够容量的目录。

### 步骤 1：安装服务端依赖

```bash
cd /home/l00899543/RL/KernelGYM
bash setup.sh
```

**发生什么**：安装 API、Redis client、GPU/任务调度等依赖，并在本机安装 Redis。详见 [3.1](#31-层-akernelgym-评测服务)。

### 步骤 2：启动 KernelGYM 环境

```bash
cd /home/l00899543/RL/KernelGYM
./start_all_with_monitor.sh
```

**发生什么**（`start_all_with_monitor.sh`）：

1. 如果没有 `.env`，调用 `scripts/auto_configure.sh` 自动生成；
2. 加载 `.env`，确认 Redis 可访问，必要时启动本地 Redis；
3. 后台启动 `python -m kernelgym.server.api.server`；
4. 后台启动 `python -m kernelgym.worker.worker_monitor --persistent`；
5. 从 `GPU_DEVICES` 取 GPU 列表，并为每张 GPU 启动 `kernelgym.worker.single_worker`。

服务启动后，以**你自己的服务地址**验证：

```bash
curl "http://<kernelgym-host>:<api-port>/health"
curl "http://<kernelgym-host>:<api-port>/workers/status"
```

期望第一个请求返回健康状态。训练入口还会在 `main_kernel.py:246–252` 主动访问 `<server-url>/health`；不健康会立刻中止。

### 步骤 3：安装训练端

```bash
cd /home/l00899543/RL/KernelGYM/drkernel
bash setup.sh
```

**发生什么**：初始化 `verl` submodule、安装 VERL 与定版的 Ray/vLLM/Torch 等，最后安装与其匹配的 FlashAttention wheel。详见 [3.2](#32-层-bdrkernel--verl-rl-训练端)。

### 步骤 4：设置非敏感运行变量

以下示例只使用占位符；把路径替换为你的真实路径。若运行 launcher 原样的 8B recipe，推荐先编辑其开头的 `TRAIN_DATASET`、`VALID_DATASET`、`MODEL_NAME` 和 `MODEL_PATH`，因为它会在脚本内直接赋值。

```bash
cd /home/l00899543/RL/KernelGYM/drkernel

export KERNELGYM_SERVER_URL="http://<kernelgym-host>:<api-port>"
export HDFS_DATA_PATH="/absolute/path/to/rl-parquet"
export HDFS_CHECKPOINT_PATH="/absolute/path/to/checkpoints"
export PROJECT_NAME="kernelgym-rl-learning"
```

**发生什么**：

- `KERNELGYM_SERVER_URL` 会传进 `reward_model.server_url`；
- `HDFS_DATA_PATH` 是相对数据集名字的补全根；
- `HDFS_CHECKPOINT_PATH` 决定 `CHECKPOINT_DIR`；
- `PROJECT_NAME` 传给 trainer 的 logger。

### 步骤 5：先做只验证，不更新参数的冒烟检查

```bash
cd /home/l00899543/RL/KernelGYM/drkernel/kernel/scripts/rl
bash 8b_trloo_mrs_pr_prs.sh --val_only True
```

**发生什么**：`--val_only True` 被 `parse_arguments()` 写入 `VAL_ONLY`，再传为 `trainer.val_only=True`。`RayKernelTrainer.fit()` 在初始验证后直接返回（`kernel_trainer.py:2750–2763`），所以它可以验证模型加载、vLLM rollout、服务奖励和验证数据链路，却不执行 actor 更新。

> 这仍然会实际生成、调用评测服务并消耗 GPU 时间；它不是零成本的语法检查。

### 步骤 6：正式训练或学习型小实验

原始 recipe：

```bash
cd /home/l00899543/RL/KernelGYM/drkernel/kernel/scripts/rl
bash 8b_trloo_mrs_pr_prs.sh
```

为了先理解数据流并降低资源消耗，可使用该 launcher 明确支持的参数缩小一次实验，例如：

```bash
bash 8b_trloo_mrs_pr_prs.sh \
  --train_batch_size 2 \
  --rollout_n 2 \
  --total_epochs 1 \
  --max_turn 2 \
  --save_freq 1 \
  --test_freq 1
```

这会大幅改变统计性质，**不应把小实验指标和论文/官方 recipe 比较**；它的目的仅是验证和学习流程。`--train_batch_size`、`--rollout_n`、`--total_epochs`、`--max_turn` 等均由 `parse_arguments()` 显式支持。

---

## 5. RL 启动脚本逐行解读：`8b_trloo_mrs_pr_prs.sh`

文件：`drkernel/kernel/scripts/rl/8b_trloo_mrs_pr_prs.sh`。14B 脚本在当前版本中除了 `MODEL_NAME` 与 `RUN_NAME` 外内容相同，因此只需理解这一份。

### 5.1 Bash 基础语法先导

| 写法 | 语法 | 作用 |
|---|---|---|
| `X=value` | Bash 变量赋值，等号两侧不能有空格 | 为当前 shell 保存字符串/数值 |
| `X="${Y:-fallback}"` | 参数展开 | `Y` 未设置或为空时取 `fallback` |
| `A=("x" "y")` | Bash 数组 | 数据集可为多个文件；后续用 `${A[@]}` 展开所有元素 |
| `"$@"` | 所有原始位置参数，且保留每个参数边界 | 将 CLI 参数原样转发给 `main` |
| `source file` | 在**当前 shell**执行另一个脚本 | 被加载脚本定义的函数/变量可继续使用 |
| `$(...)` | command substitution | 执行命令并把标准输出替换到当前位置 |
| `[ -z "$X" ]` | test 条件 | 当字符串为空时为真 |

### 5.2 数据、模型、run identity（第 3–11 行）

```bash
TRAIN_DATASET=("hkust-nlp/drkernel-rl-data")
VALID_DATASET=("hkust-nlp/drkernel-validation-data")
KERNELGYM_SERVER_URL="${KERNELGYM_SERVER_URL:-""}"
MODEL_NAME=hkust-nlp/drkernel-8b
MODEL_PATH=${MODEL_NAME}

RUN_NAME="drkernel-8b"
REWARD_MANAGER=kernel_async
REWARD_FUNC_NAME="calculate_reward_speedup"
```

逐行作用：

1. `TRAIN_DATASET=(...)`：创建含一个元素的 Bash 数组。这个字符串不是自动下载指令；公共脚本会把它解析为 parquet 路径。真实训练时改为你的路径，或通过 `--train_dataset /absolute/file.parquet` 覆盖数组。
2. `VALID_DATASET=(...)`：同理，验证集数组；可由 `--valid_dataset` 覆盖。
3. `KERNELGYM_SERVER_URL="${KERNELGYM_SERVER_URL:-""}"`：保留调用者已经 `export` 的 URL；没有时为空字符串。为空时最终 `AsyncKernelRewardManager` 会因缺少 `server_url` 报错。
4. `MODEL_NAME=...`：默认模型 ID，也参与 run 名与按参数名解析模型大小。
5. `MODEL_PATH=${MODEL_NAME}`：把模型路径明确设为上行 ID。**关键**：公共脚本优先使用非空的 `MODEL_PATH`，所以命令行的 `--model_name` 只改 `MODEL_NAME`，不会覆盖这里已设定的实际加载路径。若换模型，编辑这两行（或至少 `MODEL_PATH`）。
6. `RUN_NAME`：日志、checkpoint 子目录的基础名字。
7. `REWARD_MANAGER=kernel_async`：选择异步 Kernel 奖励管理器；会在 `main_kernel.py:237–240` 选择 `AsyncKernelRewardManager`。
8. `REWARD_FUNC_NAME="calculate_reward_speedup"`：让 `KernelRewardClient` 选按 speedup 计算 reward 的函数，而非 YAML 的基础默认 `calculate_reward_weighted`。

### 5.3 算法、奖励与 rollout correction（第 14–39 行）

```bash
ALGORITHM="trloo"
SPEEDUP_REWARD_UPPER_BOUND=3.0
SPEEDUP_REWARD_LOWER_BOUND=0.0

ROLLOUT_RS="geometric"
ROLLOUT_TOKEN_VETO_THRESHOLD=1e-4
ROLLOUT_RS_KWARGS="{lower:0.999,upper:1.001}"

COVERAGE_RS="turn"
COVERAGE_RS_THRESHOLD=0.3
COVERAGE_RS_FACTOR=0.1
COVERAGE_RS_KEY="time_coverage"

COVERAGE_REWARD_TYPE="time_coverage"
COVERAGE_REWARD_WEIGHT=0.5
COVERAGE_REWARD_ENABLE=True

REWARD_TASK_TIMEOUT=300
REWARD_TIMEOUT=1800
REWARD_ACQUIRE_TIMEOUT=2400
REWARD_MAX_CONCURRENT=32
REWARD_MAX_RETRIES=3
REWARD_PRINT_STATUS=True
NUM_PERF_TRIALS=100
REWARD_TASK_TIMEOUT_CLIENT=2400
```

| 行/变量 | 运行含义 |
|---|---|
| `ALGORITHM="trloo"` | 覆盖基础 YAML 的 `algorithm.adv_estimator: grpo`，最终进入 TRLOO 分支。|
| `SPEEDUP_REWARD_UPPER_BOUND=3.0` | 计算速度奖励时把过大的 speedup 截至 3.0，避免极端测量主导训练。|
| `SPEEDUP_REWARD_LOWER_BOUND=0.0` | speedup 低于下界时，speedup 奖励设为 0；注意这不是所有失败都会给负数，实际 penalty 由 reward policy 决定。|
| `ROLLOUT_RS="geometric"` | 启用 rollout rejection-sampling correction 的 `geometric` 方式；参数被交给算法配置。|
| `ROLLOUT_TOKEN_VETO_THRESHOLD=1e-4` | 设置 token 级 veto 阈值；它属于 rollout correction/稳定性机制，不等同于“丢弃 reward 低样本”。|
| `ROLLOUT_RS_KWARGS="{lower:0.999,upper:1.001}"` | 传给 Hydra 的字典文本，定义非常窄的几何 correction 区间。|
| `COVERAGE_RS="turn"` | 按 turn 使用 coverage 相关筛选信息。|
| `COVERAGE_RS_THRESHOLD=0.3` | coverage 低阈值。|
| `COVERAGE_RS_FACTOR=0.1` | coverage 筛选的概率/缩放因子。|
| `COVERAGE_RS_KEY="time_coverage"` | 用自定义 kernel 在 profiling 总时间中的占比，而不是 kernel 数量占比。|
| `COVERAGE_REWARD_TYPE` | 指定 coverage 的度量类型。|
| `COVERAGE_REWARD_WEIGHT=0.5` | 若 coverage reward 启用，把 coverage 乘以该权重加入最终 reward。|
| `COVERAGE_REWARD_ENABLE=True` | 覆盖 YAML 默认关闭的 coverage reward。|
| `REWARD_TASK_TIMEOUT=300` | 单个任务给服务端的执行时间限制（秒）。|
| `REWARD_TIMEOUT=1800` | HTTP/reward client 的一般 timeout 配置（秒）。|
| `REWARD_ACQUIRE_TIMEOUT=2400` | 获取异步 HTTP 提交令牌的最长等待时间（秒）。|
| `REWARD_MAX_CONCURRENT=32` | reward client 的 Ray HTTP worker 可并发任务数。|
| `REWARD_MAX_RETRIES=3` | 对可重试提交错误的最大尝试次数。|
| `REWARD_PRINT_STATUS=True` | 让奖励管理器打印每个评测任务状态，便于排错。|
| `NUM_PERF_TRIALS=100` | 速度测量试验次数；更稳定但贵。第 80 行再次赋相同值，没有额外语义。|
| `REWARD_TASK_TIMEOUT_CLIENT=2400` | 客户端总等待上限，应不小于服务端 task timeout，并还要包含排队时间。|

### 5.4 多轮、批大小、PPO 与采样（第 41–80 行）

```bash
VAL_BEFORE_TRAIN=True
IS_GET_LAST_TURN=True
ENABLE_MULTI_TURN=True
MAX_TURN=3
N_VAL=8
ACTOR_OPTIMIZER_OFFLOAD=True
ACTOR_PARAMETER_OFFLOAD=True
LEARNING_RATE=1e-6

TRAIN_BATCH_SIZE=16
PPO_MINI_BATCH_SIZE=16
AUTOMATIC_OVERSAMPLING=False
REJECTION_SAMPLE=True

PPO_MICRO_TOKEN=null
CLIP_RATIO=0.2_0.28
ENTROPY_CLIP_RATE=0.0
GRAD_CLIP=1.0
VLLM_IS_THRESHOLD=2.0
EXTREME_RISK_PROB_THRESHOLD=null
KL_LOSS_COEF=0.0
ENTROPY_COEFFIENT=0.0
KL_LOSS_TYPE="low_var_kl"
TEMPERATURE=1.0
MIN_P=0.0
TOP_P=1.0
TOP_K=-1
ROLLOUT_N=16
KL_COEF=0.0
TOTAL_EPOCHS=1000
ROLLOUT_GPU_MEMORY_UTIL=0.75
```

| 变量 | 运行含义 |
|---|---|
| `VAL_BEFORE_TRAIN=True` | 首次参数更新前先跑验证；适合得到 RL 前的 baseline。|
| `IS_GET_LAST_TURN=True` | 在 trainer 的筛选路径中抽取每条多轮轨迹最后一轮作为筛选单位；不要把它误读为“模型只生成最后一轮”。|
| `ENABLE_MULTI_TURN=True`, `MAX_TURN=3` | 启用多轮 agent rollout；每个候选最多三次模型回应/环境反馈循环。|
| `N_VAL=8` | 验证每题生成 8 个候选；它独立于训练 `ROLLOUT_N`。|
| 两个 `*_OFFLOAD=True` | FSDP 将 actor 参数/优化器状态按配置卸载以省显存；常以传输/吞吐换显存。|
| `LEARNING_RATE=1e-6` | actor optimizer 学习率。|
| `TRAIN_BATCH_SIZE=16` | 一次训练 iteration 的目标 prompt 数；与 rollout 数相乘后会膨胀。|
| `PPO_MINI_BATCH_SIZE=16` | actor update 时的大 batch 再切 mini-batch 的样本数。|
| `AUTOMATIC_OVERSAMPLING=False` | 关闭动态自动调大采样；本 recipe 的 prompt/sample factor 也都设为 1。|
| `REJECTION_SAMPLE=True` | trainer 开启过滤/拒绝采样逻辑。|
| `PPO_MICRO_TOKEN=null` | 让公共脚本按模型名中的 `8B/14B` 自动选择每 GPU token 微批预算。|
| `CLIP_RATIO=0.2_0.28` | 公共脚本拆成 PPO 下/上 clip：0.2、0.28。|
| `ENTROPY_CLIP_RATE=0.0` | 不基于 entropy 屏蔽低熵 token。|
| `GRAD_CLIP=1.0` | 用梯度范数裁剪限制更新。|
| `VLLM_IS_THRESHOLD=2.0` | 当前 `run_training()` 没有将该变量传为 Hydra override；它在这份 launcher 中没有实际生效路径，不能仅凭赋值就认为启用了。|
| `EXTREME_RISK_PROB_THRESHOLD=null` | 禁用低概率且负 advantage token 的极端风险掩码。|
| `KL_LOSS_COEF=0.0`, `KL_COEF=0.0` | 关闭 actor KL loss 和 reward 内 KL penalty，因而通常不需要 reference policy。`KL_LOSS_TYPE` 虽被传入，但系数为零时不产生 KL 项。|
| `ENTROPY_COEFFIENT=0.0` | 关闭显式 entropy bonus。变量拼写在源码就是 `COEFFIENT`。|
| `TEMPERATURE=1.0`, `MIN_P=0.0`, `TOP_P=1.0`, `TOP_K=-1` | rollout 采样分布几乎不额外截断。|
| `ROLLOUT_N=16` | 每 prompt 采样 16 个候选，提供 TRLOO 的比较组。|
| `TOTAL_EPOCHS=1000` | 训练 dataloader 外层 epoch 数；实际 step 数仍取决于数据集和跳过的 batch。|
| `ROLLOUT_GPU_MEMORY_UTIL=0.75` | vLLM 允许使用的 GPU 内存比例上限。|

### 5.5 存档、并行、上下文和入口（第 76–101 行）

```bash
SAVE_FREQ=10
TEST_FREQ=10
ROLLOUT_TENSOR_MODEL_PARALLEL_SIZE=1
SP_SIZE=4
NUM_PERF_TRIALS=100
APPLY_CHAT_TEMPLATE=True
FREE_CACHE_ENGINE=False
ENFORCE_EAGER=False
NNODES=$ARNOLD_WORKER_NUM
GPUS_PER_NODE=$ARNOLD_WORKER_GPU
if [ -z "$ARNOLD_WORKER_GPU" ]; then
    GPUS_PER_NODE=8
fi

MAX_PROMPT_LENGTH=10240
MAX_RESPONSE_LENGTH=8192
PROMPT_OVERSAMPLING_FACTOR=1.0
SAMPLE_OVERSAMPLING_FACTOR=1.0
SAMPLE_SELECTION_STRATEGY=efficiency_stochastic
MAX_SKIP_STEPS=5

source "$(dirname "$0")/train_rl_common.sh"
main "$@"
```

逐行作用：

1. `SAVE_FREQ=10`、`TEST_FREQ=10`：每 10 个 global step 保存 checkpoint、跑验证。
2. `ROLLOUT_TENSOR_MODEL_PARALLEL_SIZE=1`：vLLM rollout 不做 tensor parallel；若增大，`generate_model_micro_token()` 会相应放大 token 预算。
3. `SP_SIZE=4`：Ulysses sequence parallel size。它与 token 预算检查有关，`total_micro_token=PPO_MICRO_TOKEN×SP_SIZE`。
4. 重复的 `NUM_PERF_TRIALS=100` 只是同值重赋。
5. `APPLY_CHAT_TEMPLATE=True`：数据侧应用模型 chat template。
6. `FREE_CACHE_ENGINE=False`：不在 rollout 间主动释放 vLLM cache，通常减少重复初始化但常驻显存更多。
7. `ENFORCE_EAGER=False`：让 vLLM 使用默认的非 eager 执行策略。
8. `NNODES=$ARNOLD_WORKER_NUM`、`GPUS_PER_NODE=$ARNOLD_WORKER_GPU`：读取集群平台变量。变量为空不等于报错；公共脚本的 `${NNODES:-...}` 会回退为 1，显式 `if` 则将每节点 GPU 回退为 8。
9. `if [ -z ... ]; then ... fi`：若字符串长度为零，执行中间赋值；`fi` 结束条件块。
10. `MAX_PROMPT_LENGTH=10240`、`MAX_RESPONSE_LENGTH=8192`：单样本输入/输出 token 上限，总上限非常大，直接影响显存和吞吐。
11. 两个 `*_OVERSAMPLING_FACTOR=1.0`：这份配方不额外生成更多 prompt 或候选来抵消筛选。
12. `SAMPLE_SELECTION_STRATEGY=efficiency_stochastic`：把选择策略名字传入 data 配置。
13. `MAX_SKIP_STEPS=5`：限制/控制可因筛选等原因跳过的连续训练步数。
14. `source "$(dirname "$0")/train_rl_common.sh"`：`dirname "$0"` 得到当前 launcher 所在目录；`$(...)` 取其输出；`source` 在当前 shell 载入公共函数，**不是新进程**。
15. `main "$@"`：调用刚载入的 `main` 函数；`"$@"` 将用户在 `bash 8b... --rollout_n 2` 输入的每个参数完整保留并转交。

---

## 6. 公共启动器与 Hydra 命令逐行解读

文件：`drkernel/kernel/scripts/rl/train_rl_common.sh`。任务脚本只提供本 recipe 的值；公共脚本提供默认值、CLI 解析、路径派生和最终的 Python/Hydra 调用。

### 6.1 先发生的 shell 初始化

```bash
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/../../../setup_env.sh"
```

- `${BASH_SOURCE[0]}`：当前**被 source 文件**的路径；比 `$0` 更适合公共脚本。
- `dirname`：取目录；`cd ... && pwd`：得到规范的绝对目录。
- `source .../setup_env.sh`：导出 `DRKERNEL_ROOT`、`REPO_ROOT`、`PYTHONPATH`、`PROJECT_NAME`、`VLLM_USE_V1`、默认数据/模型/checkpoint 根目录（`drkernel/setup_env.sh:6–21`）。

接着 `NNODES=${NNODES:-${ARNOLD_WORKER_NUM:-1}}` 使用嵌套默认值：优先已有 `NNODES`，其次平台变量，最后为 1。若 `SERVER_WITH_TRAINING=true`，第 37–48 行会从训练节点数扣除留给 server 的节点数。

### 6.2 CLI 参数是如何覆盖变量的

`parse_arguments()` 在 `train_rl_common.sh:362–471` 中做：

```bash
while [[ "$#" -gt 0 ]]; do
  case "$1" in
    --rollout_n) ROLLOUT_N="$2"; shift 2 ;;
    *) echo "Unknown option: $1"; exit 1 ;;
  esac
done
```

- `"$#"` 是剩余位置参数数目；`while` 循环直到没有参数。
- `case "$1" in ... esac` 按第一个参数匹配分支。
- `ROLLOUT_N="$2"`：把第二个参数写入变量。因此 `--rollout_n 2` 生效。
- `shift 2`：丢弃刚消费的 flag 与 value；没有它会无限循环。
- 未匹配的 flag 立刻退出，避免静默拼错参数。

例如：

```bash
bash 8b_trloo_mrs_pr_prs.sh \
  --train_batch_size 2 \
  --rollout_n 2 \
  --max_turn 2 \
  --total_epochs 1
```

四个 `--flag value` 分别覆盖 shell 变量，之后统一转成 Hydra 配置。`--help`/`-h` 在解析前会打印帮助并退出。

### 6.3 模型、数据、checkpoint 和内存预算怎样派生

#### 数据路径：`format_dataset_paths()`（第 561–580 行）

```bash
if [[ "$dataset_path" == /* || "$dataset_path" == ./* || \
      "$dataset_path" == ../* || "$dataset_path" == *.parquet || \
      "$dataset_path" == *.jsonl ]]; then
  resolved_path="$dataset_path"
elif [[ -n "${HDFS_DATA_PATH}" ]]; then
  resolved_path="${HDFS_DATA_PATH}/${dataset_path}.parquet"
else
  resolved_path="${dataset_path}.parquet"
fi
```

含义：绝对路径、以 `./`/`../` 开头的路径、已经带 `.parquet`/`.jsonl` 的路径保持原样；否则追加 `.parquet`，且优先加 `HDFS_DATA_PATH` 根。输出会拼成 Hydra list 文本，如 `["/data/train.parquet"]`。

#### 模型与 checkpoint：`setup_training_environment()`（第 583–687 行）

关键规则：

```bash
if [[ -n "${MODEL_PATH:-}" ]]; then
  MODEL_PATH_RESOLVED="$MODEL_PATH"
elif [[ -n "${HDFS_MODEL_PATH}" ]]; then
  MODEL_PATH_RESOLVED="${HDFS_MODEL_PATH}/${MODEL_NAME}"
else
  MODEL_PATH_RESOLVED="$MODEL_NAME"
fi

CHECKPOINT_DIR="${HDFS_CHECKPOINT_PATH:-checkpoints}/${RUN_NAME}"
N_GPUS_PER_NODE=${N_GPUS_PER_NODE:-${GPUS_PER_NODE:-${ARNOLD_WORKER_GPU:-8}}}
```

优先级是：**显式 `MODEL_PATH` > `HDFS_MODEL_PATH/MODEL_NAME` > `MODEL_NAME` 本身**。这也是为什么上一节强调 `--model_name` 不会替换 launcher 已赋的 `MODEL_PATH`。

`generate_model_micro_token()` 会用正则 `([0-9]+)B` 从模型名提取规模：7B→8192，14B→4096，32B→2048。之后检查：

$$
\text{PPO\_MICRO\_TOKEN}\times\text{SP\_SIZE}
\geq \text{MAX\_PROMPT\_LENGTH}+\text{MAX\_RESPONSE\_LENGTH}
$$

不满足就停止，因为单条最大序列都装不进配置的 token 微批预算。

### 6.4 Hydra 配置合并的规则

入口 YAML 是 `kernel/config/kernel_trainer.yaml`。它的：

```yaml
defaults:
  - ppo_trainer
  - _self_
```

通过 `hydra.searchpath` 在 `verl_patch/trainer/code/config` 找到 `ppo_trainer.yaml`，后者再装配 actor、rollout、critic、ref、reward model 的基础默认值。最终优先级是：

```text
VERL 基础 component YAML
  < ppo_trainer.yaml
  < kernel_trainer.yaml
  < train_rl_common.sh 的 command-line overrides
```

所以看到 `kernel_trainer.yaml:15` 的 `reward_manager: kernel`、`:182` 的 `adv_estimator: grpo` 时，不能以为正式 recipe 在用它们：本 launcher 最终覆盖成 `kernel_async` 与 `trloo`。

### 6.5 `run_training()` 的命令头（第 689–692 行）

```bash
run_training() {
  sleep 3
  PYTHONUNBUFFERED=1 python -m kernel.main_kernel \
```

- 函数花括号定义 shell 函数；最后的 `}` 结束。
- `sleep 3` 是启动前固定等待 3 秒，不改变算法。
- `PYTHONUNBUFFERED=1 command` 只对紧随其后的 Python 进程生效，令 stdout/stderr 尽快输出，便于实时日志。
- `python -m kernel.main_kernel`：把 `kernel.main_kernel` 当 Python module 执行，等价于加载该模块并走其 `if __name__ == '__main__': main()`；依赖前面 `PYTHONPATH` 包含 `drkernel`。
- 每行末尾反斜杠 `\` 表示 shell 续行；所有 `key=value` 最终是一个 Python 命令的参数。

### 6.6 `run_training()` 的每一类 Hydra override（第 693–825 行）

Hydra 接受点分路径的 `key=value`。下面保留原始参数，并为**每行**给出语义；同一代码块内的注释即对应右侧每一行。

#### A. 验证、数据和动态采样（第 693–716 行）

```bash
trainer.val_before_train=$VAL_BEFORE_TRAIN                 # 是否先验证
algorithm.adv_estimator=$ALGORITHM                         # trloo
algorithm.is_get_last_turn=$IS_GET_LAST_TURN               # 筛选使用最后 turn

data.train_files=$TRAIN_FILES                             # 格式化后的训练文件 list
data.val_files=$VALID_FILES                                # 格式化后的验证文件 list
data.return_raw_chat=$RETURN_RAW_CHAT                      # async_vllm 时为 True
data.train_batch_size=$TRAIN_BATCH_SIZE                    # 每 step 目标 prompt 数
data.val_sample_size=$VAL_SAMPLE_SIZE                      # 验证取样本数
data.max_prompt_length=$MAX_PROMPT_LENGTH                  # prompt token 上限
data.max_response_length=$MAX_RESPONSE_LENGTH              # response token 上限
data.apply_chat_template=$APPLY_CHAT_TEMPLATE              # 应用 chat template
data.use_prioritized_sampling=$USE_PRIORITIZED_SAMPLING    # 是否按难度/成功率优先采样
data.update_success_rates_every=1                          # 每轮更新成功率统计
data.prompt_oversampling_factor=$PROMPT_OVERSAMPLING_FACTOR # 额外输入 prompt 倍率
data.sample_oversampling_factor=$SAMPLE_OVERSAMPLING_FACTOR # 每 prompt 额外生成倍率
data.sample_selection_strategy=$SAMPLE_SELECTION_STRATEGY  # 选择策略名
data.automatic_oversampling=$AUTOMATIC_OVERSAMPLING        # 是否自动调倍率
data.use_moderate_sampling=$USE_MODERATE_SAMPLING          # 是否偏好中等难度
data.use_refresh_sampling=$USE_REFRESH_SAMPLING            # 是否刷新采样状态
data.solverate_low=$SOLVERATE_LOW                          # 解题率下界
data.solverate_high=$SOLVERATE_HIGH                        # 解题率上界
data.solverate_mean=$SOLVERATE_MEAN                        # 解题率目标均值
data.solverate_std=$SOLVERATE_STD                          # 解题率目标标准差
trainer.fix_qwen3_chat_template=$FIX_QWEN3_CHAT_TEMPLATE   # 是否替换 Qwen3 template
```

前三行跨越 trainer、algorithm 两层。`IS_GET_LAST_TURN=True` 不会把多轮训练自动改成“只在末轮反传”；它首先影响 `fit()` 的筛选路径。真正的 advantage 计算还受到 `ADV_BY_LAST_TURN` 的影响，而这份 task launcher 没有显式设它，公共默认是 `False`。

#### B. rollout correction 与多轮控制（第 717–722 行）

```bash
+algorithm.rollout_is_kwargs=$ROLLOUT_IS_KWARGS             # 可新增的 IS 参数字典
+algorithm.rollout_rs_kwargs=$ROLLOUT_RS_KWARGS             # 可新增的 RS 参数字典
algorithm.rollout_rs=$ROLLOUT_RS                            # geometric RS 方法
algorithm.rollout_token_veto_threshold=$ROLLOUT_TOKEN_VETO_THRESHOLD # token veto 阈值
actor_rollout_ref.rollout.multi_turn.enable=$ENABLE_MULTI_TURN # 开多轮
actor_rollout_ref.rollout.multi_turn.max_user_turns=$MAX_TURN # 最大用户/反馈轮数
```

前两行的 `+` 是 Hydra “允许新增 key”的语法；没有 `+` 时，某些未先在当前合成配置声明的字段会被 Hydra 拒绝。`{lower:0.999,upper:1.001}` 会作为 OmegaConf/Hydra dict 解析，而不是普通 Python 字符串。

#### C. actor、FSDP、PPO loss（第 723–743 行）

```bash
actor_rollout_ref.model.path=$MODEL_PATH_RESOLVED           # 真正加载的模型位置
actor_rollout_ref.actor.optim.lr=$LEARNING_RATE             # actor 学习率
actor_rollout_ref.model.use_remove_padding=True             # 移除 padding 以节省计算
actor_rollout_ref.actor.ppo_mini_batch_size=$PPO_MINI_BATCH_SIZE # PPO mini-batch 大小
actor_rollout_ref.actor.use_dynamic_bsz=True                # 依据 token 数动态分微批
actor_rollout_ref.actor.ppo_max_token_len_per_gpu=$PPO_MICRO_TOKEN # 每 GPU token 预算
actor_rollout_ref.actor.use_kl_loss=$USE_KL_LOSS            # 是否加 actor/reference KL loss
actor_rollout_ref.actor.kl_loss_coef=$KL_LOSS_COEF          # KL loss 系数
actor_rollout_ref.actor.kl_loss_type=$KL_LOSS_TYPE          # KL 估计类型
actor_rollout_ref.actor.entropy_coeff=$ENTROPY_COEFFIENT    # entropy bonus 系数
actor_rollout_ref.actor.clip_ratio_high=$CLIP_RATIO_HIGH    # PPO 上侧 clip=0.28
actor_rollout_ref.actor.clip_ratio_low=$CLIP_RATIO_LOW      # PPO 下侧 clip=0.20
actor_rollout_ref.actor.entropy_clip_rate=$ENTROPY_CLIP_RATE # 低熵 token 屏蔽比例
actor_rollout_ref.actor.loss_agg_mode=$LOSS_AGG_MODE        # token loss 聚合规则
actor_rollout_ref.actor.loss_scale_factor=$LOSS_SCALE_FACTOR # 长序列 loss 缩放
actor_rollout_ref.actor.extreme_risk_prob_threshold=$EXTREME_RISK_PROB_THRESHOLD # 风险 token mask
actor_rollout_ref.actor.grad_clip=$GRAD_CLIP                # 梯度范数裁剪
actor_rollout_ref.model.enable_gradient_checkpointing=True  # activation checkpoint，省显存
actor_rollout_ref.actor.fsdp_config.param_offload=$ACTOR_PARAMETER_OFFLOAD # 参数 offload
actor_rollout_ref.actor.fsdp_config.optimizer_offload=$ACTOR_OPTIMIZER_OFFLOAD # 优化器 offload
actor_rollout_ref.actor.ulysses_sequence_parallel_size=$SP_SIZE # sequence parallel 大小
```

`loss_agg_mode=seq-mean-token-sum` 先对每条序列的 token loss 求和、再在序列间平均；对应 `core_algos.py:175–180`。这与把全 batch 所有 token 平均不同，长短 response 的权重关系也不同。

#### D. vLLM rollout 与 reference（第 744–765 行）

```bash
actor_rollout_ref.rollout.enforce_eager=$ENFORCE_EAGER      # vLLM 是否强制 eager
actor_rollout_ref.rollout.free_cache_engine=$FREE_CACHE_ENGINE # rollout 后是否释放 cache
actor_rollout_ref.rollout.temperature=$TEMPERATURE          # 训练 rollout 温度
actor_rollout_ref.rollout.top_p=$TOP_P                      # nucleus sampling 上界
actor_rollout_ref.rollout.top_k=$TOP_K                      # top-k；-1 通常表示不截断
actor_rollout_ref.rollout.min_p=$MIN_P                      # min-p 截断
actor_rollout_ref.rollout.log_prob_max_token_len_per_gpu=$LOG_PROB_MICRO_TOKEN # 计算 rollout log-prob token 预算
actor_rollout_ref.rollout.tensor_model_parallel_size=$ROLLOUT_TENSOR_MODEL_PARALLEL_SIZE # rollout TP 大小
actor_rollout_ref.rollout.name=vllm                         # backend 名字
actor_rollout_ref.rollout.mode=$ROLLOUT_MODE                # 默认 async_vllm
actor_rollout_ref.rollout.gpu_memory_utilization=$ROLLOUT_GPU_MEMORY_UTIL # vLLM 显存比例
actor_rollout_ref.rollout.n=$ROLLOUT_N                      # 每 prompt 候选数
actor_rollout_ref.rollout.val_kwargs.n=$N_VAL               # 验证候选数
actor_rollout_ref.rollout.val_kwargs.do_sample=$VAL_DO_SAMPLE # 验证是否采样
actor_rollout_ref.rollout.val_kwargs.temperature=$VAL_TEMPERATURE # 验证温度
actor_rollout_ref.rollout.val_kwargs.top_p=0.95             # 验证固定 top-p
actor_rollout_ref.rollout.val_kwargs.max_user_turns=$VAL_MAX_TURN # 验证最大轮数
actor_rollout_ref.rollout.max_num_batched_tokens=$max_num_batched_tokens # vLLM batch token 上限
actor_rollout_ref.rollout.calculate_log_probs=$CALCULATE_LOG_PROBS # 必须返回 rollout log-prob
actor_rollout_ref.ref.log_prob_max_token_len_per_gpu=$LOG_PROB_MICRO_TOKEN # ref log-prob token 预算
actor_rollout_ref.ref.fsdp_config.param_offload=True        # reference 参数 offload
actor_rollout_ref.ref.ulysses_sequence_parallel_size=$SP_SIZE\ # reference sequence parallel
```

注意最后一行反斜杠紧贴 `$SP_SIZE`。它仍作为续行字符工作。虽然命令始终配置 reference 的资源字段，`main_kernel.py:206–209` 只有在 `use_kl_in_reward` 或 `actor.use_kl_loss` 为真时才实际创建 `RefPolicy` worker；本 recipe 两者因 KL 系数为零而通常关闭。

#### E. reward 与 KernelGYM 环境（第 766–791 行）

```bash
reward_model.enable=False                                   # 不启用学习型 reward model worker
reward_model.reward_manager=$REWARD_MANAGER                 # kernel_async
reward_model.enhanced=$REWARD_ENHANCED                      # 传给奖励配置的增强开关
reward_model.use_sandbox_rate_limit=$REWARD_USE_SANDBOX_RATE_LIMIT # sandbox 限流开关
reward_model.server_url='"'$REWARD_SERVER_URL'"'             # 以引号保护 URL 后传 Hydra
reward_model.rate_limit=$REWARD_RATE_LIMIT                  # 提交令牌桶上限
reward_model.acquire_timeout=$REWARD_ACQUIRE_TIMEOUT        # 等令牌上限
reward_model.max_concurrent=$REWARD_MAX_CONCURRENT          # 异步 HTTP worker 并发
reward_model.task_timeout=$REWARD_TASK_TIMEOUT              # 服务端单任务 timeout
reward_model.task_timeout_in_client=$REWARD_TASK_TIMEOUT_CLIENT # 客户端端到端 timeout
reward_model.max_retries=$REWARD_MAX_RETRIES                # 提交重试次数
reward_model.task_timeout=$REWARD_TASK_TIMEOUT              # 同值重复赋值
reward_model.num_perf_trials=$NUM_PERF_TRIALS               # 性能试验次数
reward_model.print_status=$REWARD_PRINT_STATUS              # 打印每个结果
reward_model.reward_func_name=$REWARD_FUNC_NAME             # calculate_reward_speedup
reward_model.speedup_reward_upper_bound=$SPEEDUP_REWARD_UPPER_BOUND # speedup 截断上界
reward_model.speedup_reward_lower_bound=$SPEEDUP_REWARD_LOWER_BOUND # speedup 截断下界
reward_model.coverage_reward.reward_type=$COVERAGE_REWARD_TYPE # coverage 类型
reward_model.coverage_reward.weight=$COVERAGE_REWARD_WEIGHT # coverage 奖励权重
reward_model.coverage_reward.enable=$COVERAGE_REWARD_ENABLE # 启用 coverage 奖励
reward_model.coverage_rs=$COVERAGE_RS                       # coverage RS 粒度
reward_model.coverage_rs_threshold=$COVERAGE_RS_THRESHOLD   # coverage 阈值
reward_model.coverage_rs_factor=$COVERAGE_RS_FACTOR         # coverage RS 因子
reward_model.coverage_rs_key=$COVERAGE_RS_KEY               # 使用 time_coverage
reward_model.speedup_threshold=$SPEEDUP_THRESHOLD           # 可选 speedup 筛选阈值
reward_model.detect_decoy_kernel=$DETECT_DECOY_KERNEL       # 传递 decoy 检测开关
```

`reward_model.enable=False` 很容易误解：它仅关闭**神经网络 reward model worker**，并不关闭规则/环境 reward。`main_kernel.py` 依然实例化 `AsyncKernelRewardManager`，它会访问 KernelGYM API。

`reward_model.server_url='"'$REWARD_SERVER_URL'"'` 是 Bash 单引号、双引号、变量展开的组合，目标是向 Hydra 交付一个被引号包裹的 URL；这样 URL 中若有 `:`、`/` 等，解析更稳妥。不要为了“更简洁”随意删去引号。

#### F. 算法、日志、资源、checkpoint 和过滤（第 792–825 行）

```bash
algorithm.reward_shaping=$REWARD_SHAPING                    # reward 是否做 shaping
algorithm.unbiased_shaping=$UNBIASED_SHAPING                # shaping 的无偏处理
algorithm.adv_estimator=${ALGORITHM:-grpo}                  # 再次确保 estimator=trloo
algorithm.use_kl_in_reward=$USE_KL_COEF                     # 是否在 reward 中减 KL
algorithm.kl_ctrl.kl_coef=$KL_COEF                          # reward 内 KL 系数
algorithm.batch_std=${BATCH_STD:-False}                     # 是否 batch 标准化 advantage
algorithm.adv_by_last_turn=$ADV_BY_LAST_TURN                # 是否仅末轮算 advantage
algorithm.use_final_reward=$USE_FINAL_REWARD                # 是否只给末轮环境奖励
algorithm.gamma=$GAMMA                                      # 多轮折扣因子
critic.ppo_micro_batch_size_per_gpu=4                       # critic 微批；本 recipe 无 critic 价值学习需求时影响有限
trainer.critic_warmup=0                                     # actor 从第 0 step 即可更新
trainer.logger=['console','wandb']                           # 两种 logger
trainer.rejection_sample=$REJECTION_SAMPLE                  # 开启 rejection/filter
trainer.project_name=$PROJECT_NAME                          # 项目名
trainer.experiment_name=$RUN_NAME                           # 本次实验名
trainer.n_gpus_per_node=$N_GPUS_PER_NODE                    # 每训练节点 GPU 数
trainer.nnodes=$NNODES                                      # 训练节点数
trainer.remove_clip=$REMOVE_CLIP                            # 过滤/裁剪相关开关
trainer.rollout_data_dir=$ROLLOUT_DATA_DIR                  # 可选 rollout dump 目录
trainer.validation_data_dir=$VALIDATION_DATA_DIR            # 可选验证 dump 目录
trainer.log_val_generations=10                              # 记录 10 条验证生成
trainer.save_freq=$SAVE_FREQ                                # checkpoint 间隔
trainer.test_freq=$TEST_FREQ                                # 验证间隔
trainer.default_local_dir=$CHECKPOINT_DIR                   # checkpoint 目录
trainer.total_epochs=$TOTAL_EPOCHS                          # 外层 epoch 数
trainer.val_only=$VAL_ONLY                                  # 只验证不训练
trainer.max_skip_steps=$MAX_SKIP_STEPS                      # 最大 skip 控制
rejection_sampling.enable_two_gate_filter=$ENABLE_TWO_GATE_FILTER # 双 gate 总开关
rejection_sampling.gate1.enabled=$GATE1_ENABLED             # gate1 开关
rejection_sampling.gate1.bias_epsilon=$GATE1_BIAS_EPSILON   # gate1 偏差阈值
rejection_sampling.gate2.enabled=$GATE2_ENABLED             # gate2 开关
rejection_sampling.gate2.instability_threshold=$GATE2_INSTABILITY_THRESHOLD # gate2 阈值
rejection_sampling.log_rejected_samples=$LOG_REJECTED_SAMPLES # 记录被拒样本
rejection_sampling.save_rejection_stats=$SAVE_REJECTION_STATS # 存筛选统计
```

**一个当前源码细节**：公共脚本定义了 `ENABLE_TWO_GATE_FILTER`、`GATE1_BIAS_EPSILON`、`GATE2_INSTABILITY_THRESHOLD`，但当前读取范围内没有给 `GATE1_ENABLED` 和 `GATE2_ENABLED` 赋默认值。若 shell 中未预先定义，它们会展开为空，Hydra 对这两个 `enabled=` 参数的行为取决于配置/解析器。要使用双 gate 前，先检查这一点并显式导出/修正脚本；本主 recipe 的 `ENABLE_TWO_GATE_FILTER=False`，所以不应把双 gate 当作已启用的核心机制。

---

## 7. 命令启动后：配置、Ray 与训练器如何建立

入口是 `drkernel/kernel/main_kernel.py`。

### 7.1 Hydra 接住命令行覆盖

```python
@hydra.main(config_path='config', config_name='kernel_trainer', version_base=None)
def main(config):
    run_ppo(config)
```

- `@hydra.main(...)`：Hydra decorator。它读取 `kernel/config/kernel_trainer.yaml`，合并 defaults，再解析上一节的 `key=value`。
- `config`：最终的 OmegaConf 配置对象，不是手写 argparse namespace。
- `run_ppo(config)`：进入训练启动逻辑。

启动时 `TaskRunner.run()` 会打印 `OmegaConf.to_container(config, resolve=True)`（`main_kernel.py:114–122`）。这是理解“最后到底生效什么”的最佳证据：不要只看 YAML 或 shell 变量，应以打印出来的 resolved config 为准。

### 7.2 Ray 初始化与 remote runner

```python
os.environ["ENSURE_CUDA_VISIBLE_DEVICES"] = os.environ.get('CUDA_VISIBLE_DEVICES', '')
if not ray.is_initialized():
    ray.init(runtime_env={
        'env_vars': {
            'TOKENIZERS_PARALLELISM': 'true',
            'NCCL_DEBUG': 'WARN',
            'VLLM_LOGGING_LEVEL': 'WARN'
        }
    })

runner = TaskRunner.remote()
ray.get(runner.run.remote(config))
```

逐行理解：

1. `ENSURE_CUDA_VISIBLE_DEVICES`：保留当前 job 可见 GPU 范围，帮助协调 Ray 设备隔离。
2. `ray.is_initialized()`：避免重复初始化。
3. `ray.init(...)`：本地启动/连接 Ray runtime，并向 worker 注入 tokenizer、NCCL、vLLM 的环境变量。
4. `TaskRunner.remote()`：创建一个 Ray actor；类上标记 `@ray.remote(num_cpus=1)`，因此其 driver 工作在 Ray 中。
5. `runner.run.remote(config)`：异步发起远程方法调用，返回 object ref。
6. `ray.get(...)`：主进程阻塞等待；这使 shell 命令在训练结束/失败前不会返回。

### 7.3 模型、tokenizer、worker role 与资源池

`TaskRunner.run()` 的关键步骤（`main_kernel.py:124–300`）：

1. `copy_to_local(config.actor_rollout_ref.model.path)`：将模型路径规范化/下载到本地可加载位置；
2. `hf_tokenizer(...)` 与 `hf_processor(...)`：加载 tokenizer 和可选多模态 processor；
3. 读取 `actor.strategy`。当前常用 `fsdp`，导入 patch 的 `ActorRolloutRefWorker`、`AsyncActorRolloutRefWorker`、`CriticWorker`；
4. 因 `rollout.mode=async_vllm`，选择 `AsyncActorRolloutRefWorker`；
5. 建立 role 映射：ActorRollout 与 Critic；只有启用 KL 时才附加 RefPolicy；只有 `reward_model.enable=True` 才附加 neural RewardModel worker；
6. 将 `[n_gpus_per_node] * nnodes` 交给 `ResourcePoolManager`，形成 Ray 资源规格；
7. 选择 `kernel_async` 奖励管理器、动态加载 `kernel/rewards/kernel_reward.py:compute_kernel_reward_batch`；
8. 在真正训练前检查 `<server_url>/health`；
9. 构造训练/验证各一个 reward function，构造 `RayKernelTrainer`；
10. `trainer.init_workers()` 分配/初始化 worker；`trainer.fit()` 进入循环。

这就是为什么 `reward_model.enable=False` 不会关闭环境 reward：该配置只跳过第 5 步的**模型型** RewardModel role；第 7 步仍明确构造规则/HTTP 型 `AsyncKernelRewardManager`。

---

## 8. Rollout：模型怎样完成三轮“生成—反馈—改进”

### 8.1 trainer 每个 step 的骨架

`RayKernelTrainer.fit()` 的主循环位于 `kernel/kernel_trainer.py:2777–3555`。简化后的真实顺序：

```python
for batch_dict in self.train_dataloader:
    batch = DataProto.from_single_dict(batch_dict)
    batch.non_tensor_batch["uid"] = unique_ids()

    gen_batch = batch.pop(prompt_fields)
    gen_batch.meta_info["n"] = ROLLOUT_N
    gen_batch_output = self.async_rollout_manager.generate_sequences(gen_batch)

    batch = batch.repeat(ROLLOUT_N * MAX_TURN, interleave=True)
    batch = batch.union(gen_batch_output)
    batch.batch["response_mask"] = compute_response_mask(batch)

    # 后续：奖励、筛选、advantages、actor update
```

关键对象：

- `DataProto`：VERL 在 worker 之间传递的 batch 容器，包含 tensor 与非 tensor 元数据；
- `uid`：每个原始 prompt 的唯一标识。复制后的同 prompt/候选/turn 必须保留对应 uid，TRLOO 靠它分组；
- `input_ids`、`attention_mask`、`position_ids`：输入 token / 有效 token mask / position 信息；
- `responses`：模型生成 token；
- `response_mask`：只标识有效 response token，padding 不应进入 loss；
- `loss_mask`：多轮时屏蔽 void turn 等不可训练 turn。

### 8.2 为什么要 `ROLLOUT_N × MAX_TURN` 对齐

生成 worker 产出的不是一条回答：每个 prompt 的 `n=16` 个候选，且每候选最多 3 个 turn。原始 batch 只有 16 个 prompt，无法直接与输出逐行拼接。因此训练器：

```python
batch = batch.repeat(
    repeat_times=self.config.actor_rollout_ref.rollout.n * max_turns,
    interleave=True,
)
batch = batch.union(gen_batch_output)
```

（`kernel_trainer.py:2900–2936`）把任务元数据复制到对应的每条 turn record，再把生成 token、turn index、reward 信息合并进来。

如果 async rollout 超时或筛掉部分 prompt，代码以 `uid` 过滤原 batch，避免“某题的 reference code 与另一题生成结果”错位（`:2914–2934`）。这是 RL 工程中非常关键的数据对齐保护。

### 8.3 多轮 KernelAgent 的含义

基础多轮配置在 `kernel/config/kernel_trainer.yaml:90–170`：

- `multi_turn.enable: True`；
- `max_user_turns: 3`；
- `calculate_log_probs: True`；
- `agent_type: KernelAgent`；
- `prompt_config_path: kernel/config/prompt_config/multi_turn_kernel.yaml`；
- `mask_void_turn: True`。

prompt 配置的核心内容（`multi_turn_kernel.yaml:11–24`）是：当有 server feedback 时，告诉模型根据上一次实现与状态/指标/错误信息改进，并返回名为 `ModelNew` 的单一 Python 代码块。

因此一条典型轨迹是：

```text
turn 0: prompt → 模型生成第一个 Triton kernel
        ↓
        KernelGYM 返回编译错误、正确性、speedup、profiling 等 feedback
turn 1: 反馈 + 历史 → 模型修正 kernel
        ↓
        再次评测
turn 2: 再次反馈 + 历史 → 模型继续改进
```

这和普通单轮 SFT 的区别在于：模型后续 token 的条件不仅是原 prompt，还包含它先前动作造成的环境反馈。`max_user_turns` 限制的是这类反馈/交互的次数上界。

### 8.4 为什么 rollout 必须记录 log probability

PPO 要比较当前参数下某个已采样 token 的概率，和**采样它时旧策略**的概率：

$$
r_t(\theta)=\frac{\pi_\theta(a_t\mid s_t)}{\pi_{\text{old}}(a_t\mid s_t)}
=\exp(\log\pi_\theta-\log\pi_{\text{old}})
$$

因此 `calculate_log_probs=True` 是关键配置。训练器可再用 actor 重算 `old_log_probs`（`kernel_trainer.py:3002–3017`）；某些 rollout correction mode 可以直接用 `rollout_log_probs` 代替，避免额外前向计算。没有旧 log-prob，标准 PPO ratio 无从计算。

---

## 9. Reward：KernelGYM 如何把代码变成数值奖励

### 9.1 调用链

```text
AsyncKernelRewardManager.__call__()
  └─ execute_env()
       └─ compute_kernel_reward_batch()
            └─ KernelRewardClient.compute_batch_rewards()
                 └─ Ray HTTP worker
                      POST /evaluate
                      GET  /status/{task_id}
                      GET  /results/{task_id}
                           └─ KernelGYM API → workflow → Redis queue → GPU worker
```

对应文件：

- manager：`kernel/workers/reward_manager/kernel_async.py:30–339`；
- 自定义 reward function：`kernel/rewards/kernel_reward.py:94–203`；
- HTTP client 与 reward 公式：`kernel/rewards/reward_client.py:56–148, 445–543`；
- API endpoint：`kernelgym/server/api/server.py:435–461, 592–673`。

### 9.2 manager 怎样把分数放回 token 序列

`AsyncKernelRewardManager.__call__()` 的简化逻辑：

```python
valid_response_length = min(len(response_ids), response_length)
reward_tensor = torch.zeros(valid_response_length)
results = self.execute_env(response_str, ground_truth, entry_point, uuid, response_ids)
score = results[0].get("score", results[0].get("reward", 0.0))
reward_tensor[valid_response_length - 1] = score
```

这表示 reward 是**终局/结果型**的：只有 response 的最后一个有效 token 得到标量 score，其余 token 为零。之后 return/advantage 会把这个终局信号归因到整段 response 的 token。

manager 同时返回辅助信息：`correctness`、`performance/speedup`、`compilation`、`time_coverage`、`num_coverage`、`decoy_kernel`、`status` 与错误文本。它们用于 metrics、筛选和调试，不全是 policy loss 的直接奖励。

### 9.3 任务请求和异步 HTTP

`compute_kernel_reward_batch()`：

1. 对每条输出用 `extract_kernel_code()` 抽出候选实现；
2. 从数据取 `ground_truth` 作为 reference code；
3. 打包 `reference_code`、`kernel_code`、`entry_point`、`uuid`、正确性/性能 trial 数、timeout、profiling 等；
4. 重用全局 `KernelRewardClient`；
5. 通过 asyncio loop 调 `compute_batch_rewards()`。

HTTP worker 的 `submit_and_poll()` 做的不是一次同步评测：

```text
获取 token-bucket 名额
  → POST /evaluate
  → 立即释放提交名额
  → 每秒 GET /status/{task_id}
  → completed 时 GET /results/{task_id}
  → failed / timeout / cancelled 时返回失败结果
```

这使“提交任务并发度”与“服务端实际排队/运行任务数”分离，防止训练端在瞬间压垮服务端。`429`、`503`、连接/读超时会按配置重试；client timeout 覆盖排队加执行总时间。

### 9.4 KernelGYM API 做了什么

`POST /evaluate` 接收 `EvaluationRequest`（`server.py:435–461`），调用 `_execute_workflow()`，默认 workflow 为 `kernelbench`。根据根 README 的架构说明，工作流会协调：

1. backend 编译/加载候选 kernel；
2. toolkit 用参考实现做 correctness checking；
3. 测量 reference 和候选 kernel 的性能；
4. 可选 profiling；
5. TaskManager 用 Redis 队列调度；
6. GPU worker 在隔离子进程中执行。

调用方随后用 `/status/{task_id}` 与 `/results/{task_id}` 取得终态和详细结果。

### 9.5 `calculate_reward_speedup` 的真实公式

本 recipe 指定的是 `KernelRewardClient.calculate_reward_speedup()`（`reward_client.py:445–543`）。设：

- $c\in\{0,1\}$：correctness；
- $s$：服务返回的 speedup；
- $u$：`speedup_reward_upper_bound`（此 recipe 为 3.0）；
- $l$：`speedup_reward_lower_bound`（此 recipe 为 0.0）；
- $w_c$、$w_s$：初始 correctness/performance 权重，基础 YAML 分别为 0.5、0.5；
- $q$：coverage；
- $w_q$：coverage weight（此 recipe 为 0.5）。

代码逻辑相当于：

$$
s' = \begin{cases}
0 & s < l \\
\min(s,u) & s\ge l
\end{cases}
$$

$$
R_{base}=w_c\cdot c+w_s\cdot s'
$$

若 `correctness=True` 且 coverage reward 开启：

$$
R=R_{base}+w_q\cdot q
$$

否则 $R=R_{base}$。

注意事项：

- 任务未 `completed` 时，函数返回 `penalty_score`（基础 YAML 中是 0.0）；
- 若结果携带 `decoy_kernel=True`，当前代码也强制返回 penalty；
- `speedup` 为 `None` 时按 0 处理；
- `speedup` 是原始结果字段，返回 metrics 时仍保留原值，reward 使用的是截断后值；
- YAML 中虽定义 compilation/correctness/perf degradation penalty 等项，但当前 `calculate_reward_speedup` 的失败分支直接使用 `penalty_score`。不要仅凭 YAML 中未被此函数引用的字段推断真实 reward。

### 9.6 timeout 的三层含义

| 配置 | 当前值 | 意义 |
|---|---:|---|
| `reward_model.task_timeout` | 300 s | 单次评测任务向服务端声明的执行 timeout |
| `reward_model.timeout` | 1800 s | reward client 的一般 HTTP timeout 配置 |
| `reward_model.task_timeout_in_client` | 2400 s | 客户端等待提交、排队、执行和取结果的总时限 |
| `reward_model.acquire_timeout` | 2400 s | 等待提交令牌的上限 |

通常要保持：`task_timeout_in_client ≥ task_timeout`，并为排队留余量。否则服务端还未超时，训练端已经将样本按 client timeout 当成失败，导致大量无效 reward。

---

## 10. TRLOO：为什么需要 advantage，项目如何计算它

### 10.1 reward、return、baseline、advantage 的区别

- **reward** $r_t$：环境在第 $t$ 个 turn 给的即时结果分数；本项目常为终局 score。
- **return** $G_t$：从当前 turn 向后的累计回报：

$$
G_t=r_t+\gamma r_{t+1}+\gamma^2r_{t+2}+\cdots
$$

- **baseline** $b_t$：不依赖当前动作的比较值，用于降低梯度方差。
- **advantage** $A_t=G_t-b_t$：这个动作/轨迹是否比“可比候选”更好。

如果 `A>0`，PPO 倾向提升这条 response token 概率；若 `A<0`，倾向降低。

### 10.2 RLOO 与 TRLOO 的关系

普通 RLOO（REINFORCE Leave-One-Out）对同一 prompt 的 N 条完整轨迹使用：

$$
A_i=G_i-\frac{1}{N-1}\sum_{j\ne i}G_j
$$

这避免让第 i 条样本参与自己的 baseline。若把自己也算进均值，会导致 correlated baseline，梯度估计性质不同。

**TRLOO = turn-aware RLOO**。它将可比集合进一步限制为：

```text
同一个 uid（同一道原始 prompt）
+ 同一个 turn_indices（都在第 0/1/2 轮）
+ loss_mask 为 1（不是 void/padded turn）
```

理由：第 0 轮没有环境反馈，第 2 轮看到两轮反馈；它们处在不同状态分布中，不能直接混成一个 baseline。

### 10.3 当前项目的精确实现

训练器在 `compute_multi_turn_advantage()` 的 `adv_estimator == "trloo"` 分支（`kernel_trainer.py:670–685`）调用：

```python
advantages, returns = core_algos.compute_multi_turn_rloo_outcome_advantage(
    token_level_rewards=data.batch["token_level_rewards"],
    eos_mask=data.batch["response_mask"],
    loss_mask=data.batch["loss_mask"],
    turn_indices=data.batch["turn_indices"],
    index=data.non_tensor_batch["uid"],
    max_turns=max_turns,
    gamma=gamma,
)
```

底层函数是 `verl_patch/trainer/code/ppo/core_algos.py:405–475`：

```python
scores = token_level_rewards.sum(dim=-1)
returns = compute_multi_turn_returns(scores, gamma, max_turns)

for i in range(bsz):
    if turn_indices[i].item() == -1 or not loss_mask[i]:
        continue
    key = (index[i], turn_indices[i].item())
    id2return[key].append(returns[i])

# 对同 key 的 N 条样本
loo_mean_i = (sum(G_j) - G_i) / (N - 1)
advantages[i] = G_i - loo_mean_i
advantages = advantages.unsqueeze(-1).tile([1, response_length]) * eos_mask
```

源码写成等价的缩放形式：

$$
A_i=G_i\frac{N}{N-1}-\overline G\frac{N}{N-1}
=G_i-\frac{\sum_{j\ne i}G_j}{N-1}
$$

其中 $\overline G$ 是包含自身的组平均值。只有一条可用样本时，代码使用 `A_i=G_i`，因为无法构造 LOO baseline。

最后一行 broadcast 很重要：TRLOO 是 sequence/turn 级 advantage，但语言模型需要对每个 token 做 policy gradient。因此将同一个标量铺到该 response 的所有有效 token，再用 `eos_mask` 清除 padding。

### 10.4 gamma=1.0 对三轮回报意味着什么

当前 launcher 设 `GAMMA=1.0`。若某条轨迹有三轮即时 reward $r_0,r_1,r_2$：

$$
G_0=r_0+r_1+r_2,\quad G_1=r_1+r_2,\quad G_2=r_2
$$

这允许晚一轮的修正结果影响前一轮的 credit。若设置 $\gamma<1$，更晚的 reward 对早期 turn 的影响会衰减。注意 `USE_FINAL_REWARD=False` 与 `ADV_BY_LAST_TURN=False` 都来自公共默认值：本 recipe 不会仅因 `IS_GET_LAST_TURN=True` 就自动改成“只末轮 reward/advantage”。

### 10.5 与 GRPO / RLOO 的比较

| 方法 | baseline 的比较组 | 是否 leave-one-out | 是否区分 turn | 当前 recipe |
|---|---|---:|---:|---:|
| GRPO | 同 prompt 的候选均值/标准差 | 否 | 常规版本不专门区分 | 否（被覆盖） |
| RLOO | 同 prompt 的其他候选 | 是 | 单轮/完整 trajectory 级 | 否 |
| TRLOO | 同 prompt、同 turn 的其他候选 | 是 | 是 | 是 |

TRLOO 的好处是多轮状态下比较更公平、variance 更低；代价是每个 `(prompt, turn)` 都需至少多个有效 rollout。若大量候选超时/被 mask，LOO 组变小，优势估计会更嘈杂。

---

## 11. PPO：advantage 如何真正改动模型参数

### 11.1 PPO 的核心直觉

给定一个已产生的 response token $a_t$，旧策略概率为 $\pi_{old}$，当前要更新的策略概率为 $\pi_\theta$：

$$
r_t(\theta)=\exp(\log\pi_\theta(a_t|s_t)-\log\pi_{old}(a_t|s_t))
$$

不加限制的 policy gradient 会最大化 $r_tA_t$，可能一步把概率推得过远。PPO 将 ratio 限制在区间附近：

$$
L^{CLIP}=\min\left(r_tA_t,\operatorname{clip}(r_t,1-\epsilon_l,1+\epsilon_h)A_t\right)
$$

实现用的是要最小化的负 loss，并且使用 dual clipping 对负 advantage 做额外保护。

### 11.2 trainer 何时调用 actor 更新

在 `kernel_trainer.py:3505–3555`，顺序为：

1. 若启用 critic，先 `update_critic(batch)`；
2. 计算全局有效 token 数，供分布式统计；
3. `critic_warmup=0` 后即可调用 `self.actor_rollout_wg.update_actor(batch)`；
4. 按 `test_freq` 验证、按 `save_freq` checkpoint；
5. 计算并记录 metrics。

本 recipe 的 advantage 是 outcome/relative 形式，不需要像 GAE 那样依赖 learned value 才能工作；不过框架仍保有 critic 相关通路。

### 11.3 `update_policy()` 的逐步逻辑

实现位于 `verl_patch/workers/code/actor/dp_actor.py:445–681`：

```python
self.actor_module.train()
batch = data.select([... "old_log_probs", "advantages"]).batch

dataloader = batch.split(self.config.ppo_mini_batch_size)
for epoch in range(self.config.ppo_epochs):
    for mini_batch in dataloader:
        micro_batches = rearrange_micro_batches(...)
        self.actor_optimizer.zero_grad()
        for micro_batch in micro_batches:
            entropy, log_prob, _ = self._forward_micro_batch(...)
            policy_output = core_algos.compute_policy_loss(
                old_log_prob=old_log_prob,
                log_prob=log_prob,
                advantages=advantages,
                eos_mask=response_mask,
                cliprange_low=0.2,
                cliprange_high=0.28,
                ...,
            )
            policy_loss = pg_loss - entropy_loss * entropy_coeff
            loss.backward()
        grad_norm = self._optimizer_step()
```

逐项理解：

- `train()`：打开训练模式；
- `old_log_probs`：rollout 时策略对已采样 token 的 log probability；
- `advantages`：TRLOO 产生的学习方向；
- `dataloader`：一个 PPO batch 再切成 `ppo_mini_batch_size`；
- `micro_batches`：按 token 数继续切分，使可变长度 response 不 OOM；
- `_forward_micro_batch()`：当前参数对相同 token 序列前向，得到 `log_prob` 与 entropy；
- `compute_policy_loss()`：构造 ratio、PPO/dual clipping、可选 IS 权重；
- `loss.backward()`：累积当前 micro-batch 梯度；
- `_optimizer_step()`：执行 optimizer 更新并返回 grad norm。

### 11.4 当前 dual-clip PPO 的具体实现

`core_algos.compute_policy_loss()`（`core_algos.py:794–892`）的核心：

```python
negative_approx_kl = log_prob - old_log_prob
ratio = torch.exp(negative_approx_kl)

pg_losses_original = -advantages * ratio
pg_losses2 = -advantages * torch.clamp(
    ratio, 1.0 - cliprange_low, 1.0 + cliprange_high
)
clip_pg_losses1 = torch.maximum(pg_losses_original, pg_losses2)

pg_losses3 = -advantages * clip_ratio_c
clip_pg_losses2 = torch.minimum(pg_losses3, clip_pg_losses1)
pg_losses = torch.where(advantages < 0, clip_pg_losses2, clip_pg_losses1)
```

本配方的 `cliprange_low=0.20`、`cliprange_high=0.28`，所以普通 ratio 约束范围是 $[0.8,1.28]$。它是不对称的：允许正向概率增长的空间略大于下降空间。`clip_ratio_c` 默认 3.0，只在 advantage 为负时参与 dual clipping，避免特别坏样本导致过强更新。

最终 loss 还可能加：

$$
L=L_{PG}-c_HH+c_{KL}KL
$$

不过当前 recipe `ENTROPY_COEFFIENT=0.0`、`KL_LOSS_COEF=0.0`，所以实际主要由 policy gradient loss 构成；仍会记录 entropy 等诊断指标。

### 11.5 loss 聚合为何重要

`agg_loss()` 支持多种聚合。当前 `seq-mean-token-sum`：

$$
L=\frac{1}{B}\sum_{i=1}^{B}\sum_{t\in\text{valid}(i)}l_{i,t}
$$

即每条序列 token loss 先求和，再让每条**序列**在 batch 平均时权重相同。相较全 token mean，它减少“单纯因生成更长而在 batch 中占更多 token”的影响。长序列又用 `LOSS_SCALE_FACTOR=1000.0` 缩放最终 loss，防止梯度数值尺度失控。

---

## 12. MRS、PR、PRS、筛选与采样：本配方到底启用了什么

README 将该命令称为 `TRLOO + MRS + PR + PRS` recipe。就**当前脚本中可直接观察到的开关**，可以可靠地解释为以下工程动作；不要在未对照论文/项目说明时凭缩写扩展出额外算法定义。

### 12.1 已明确启用的内容

| 配置 | 当前值 | 可从源码确认的行为 |
|---|---:|---|
| `algorithm.adv_estimator` | `trloo` | 使用 turn-aware leave-one-out advantage |
| `algorithm.rollout_rs` | `geometric` | 启用 rollout correction 的 rejection-sampling 模式 |
| `rollout_rs_kwargs` | `{lower:0.999,upper:1.001}` | 传给该 correction 的上下界 |
| `rollout_token_veto_threshold` | `1e-4` | token 级 veto 阈值传入算法配置 |
| `trainer.rejection_sample` | `True` | trainer 可执行 batch filtering / rejection 流程 |
| `coverage_rs` | `turn` | 以 turn 级 coverage 信息参与筛选配置 |
| `coverage_reward.enable` | `True` | 正确样本的 reward 加入 coverage 项 |
| `prompt/sample_oversampling_factor` | `1.0 / 1.0` | 本 recipe 未额外过采样弥补被拒样本 |
| `automatic_oversampling` | `False` | 不动态调大采样倍率 |

`fit()` 在得到 token level scores 后调用 batch filter（`kernel_trainer.py:3143–3284`）：它带着每条样本的总 reward、response mask、以及可能的 old/rollout log-prob、top log-prob、prompt index 做选择。若筛选后不足目标 batch，代码把样本存入 buffer 并跳过这一步（`:3286–3338`）。因此 RL 实际吞吐不只由 `TRAIN_BATCH_SIZE×ROLLOUT_N` 决定，也取决于评测成功率和筛选保留率。

### 12.2 三种容易混淆的“采样”

1. **模型 sampling**：`temperature/top_p/top_k/min_p` 决定 vLLM 从 token 分布怎样抽样，产生不同候选。
2. **rollout correction（IS/RS）**：修正 rollout 策略与训练时策略/数值实现之间的概率失配；与“reward 高低”不是同一概念。
3. **rejection/filtering**：根据 reward、coverage、稳定性或其他质量信号不让某些 group 进入更新；这会减少有效 batch，需要 buffer/oversampling 配合。

把它们混为“拒绝采样”会很难理解日志。排错时应分别查看生成数、评测完成数、筛选后数量、最终有效 response token 数。

---

## 13. 训练观察、缩小实验与排错

### 13.1 建议的学习顺序

1. **读配置，不运行**：先用 `--help` 看公共 launcher 支持项，并阅读 shell 打印出的 resolved model/data/checkpoint 路径。
2. **服务检查**：确认 `/health`、`/workers/status`，查看服务端 logs 目录。
3. **`--val_only True`**：验证模型加载、数据、rollout、HTTP 奖励。
4. **极小训练**：2 prompt × 2 rollout × 1 epoch（或你的资源可承受的最小完整比较组）；保留每组至少两个 rollout，否则 TRLOO 不能有真正 LOO baseline。
5. **再回到官方尺度**：逐步增大 batch、`ROLLOUT_N`、长度与并发，不要一次同时改所有变量。

### 13.2 优先观察的指标

| 指标类别 | 为什么看 | 异常通常说明什么 |
|---|---|---|
| reward / score | 环境回报是否有学习信号 | 长期全为 penalty/0：服务、代码抽取、数据字段、timeout 或 reward 配置问题 |
| correctness / compiled | 模型输出是否能构建并语义正确 | 低：先加强 SFT、检查 prompt/template/代码块格式 |
| speedup / coverage | 性能目标是否真在提升 | correctness 高而 speedup 不升：reward 权重、任务难度或优化能力问题 |
| `rollout_timeout_samples` | async rollout 被筛掉的数量 | 高：vLLM/长度/服务吞吐不足 |
| 筛选前后有效 sample 数 | 是否仍有足够 TRLOO 比较组 | 低：增大 rollout 或调整筛选/over-sampling |
| `actor/clip_fraction` | PPO 是否频繁触及 clip | 很高：学习率/优势尺度/更新 epoch 过激；接近 0 不一定坏，但需结合 reward |
| `actor/avg_kl`、`actor/avg_entropy` | 分布漂移和探索程度 | KL 激增或 entropy 塌缩：更新不稳定；当前 KL 不进 loss，只是诊断 |
| `actor/grad_norm` | 反向梯度规模 | 爆炸：检查 reward 异常值、loss scale、grad clip、长度 |
| checkpoint 与 validation | 是否可恢复、是否真实泛化 | 只看 train reward 容易被特定任务/漏洞误导 |

### 13.3 常见问题与定位

| 症状 | 首先检查 | 相关代码/配置 |
|---|---|---|
| 一开始就 `server_url is required` | 是否 export 了 `KERNELGYM_SERVER_URL` | launcher:5；`kernel_async.py:63–70` |
| health check 失败 | API 是否启动、URL/端口/网络是否可达 | `main_kernel.py:56–88` |
| 数据找不到 | 相对 dataset name 是否被补成意外的 `.parquet` 路径 | `train_rl_common.sh:561–580` |
| 改 `--model_name` 仍加载旧模型 | launcher 已将 `MODEL_PATH=${MODEL_NAME}` 固定 | launcher:6–7；common:610–616 |
| OOM | 先降低 prompt/response length、rollout N、GPU memory utilization，检查 FSDP/vLLM GPU 是否冲突 | common:660–676，run_training rollout 配置 |
| 大量 client timeout | 对照 server task timeout、client total timeout、队列积压和 concurrency | `reward_client.py:56–148` |
| 很多样本被跳过 | 查看 filter 后样本数与 `MAX_SKIP_STEPS` | `kernel_trainer.py:3143–3338` |
| W&B 登录/上传失败 | 设置凭据或按组织要求修改 logger；不要把 key 写入 launcher | `train_rl_common.sh:803` |
| 双 gate 参数解析异常 | 当前 common script 未初始化 `GATE1_ENABLED/GATE2_ENABLED` | `train_rl_common.sh:819–823` |

### 13.4 对“训练是否成功”的严格标准

不能只看到 process 未报错或 loss 下降。至少确认：

- 模型成功加载、KernelGYM `/health` 为 healthy；
- 有非零/非全相同的 rollout reward；
- 有足够的同 `(uid, turn)` 多样本组让 TRLOO LOO baseline 有意义；
- actor update 实际执行，能看到 grad norm、policy loss/clip metrics；
- 正确性与性能分开观察，尤其不能用错误代码的虚假 speedup 判断成功；
- 在 validation tasks 上观察到合理表现并生成 checkpoint。

---

## 14. 一轮训练的完整调用链速查

```text
shell
  kernel/scripts/rl/8b_trloo_mrs_pr_prs.sh
    ├─ 定义 recipe 变量
    ├─ source train_rl_common.sh
    └─ main "$@"

shell common layer
  train_rl_common.sh
    ├─ source ../../../setup_env.sh
    ├─ parse_arguments → 覆盖变量
    ├─ setup_training_environment → 路径、GPU、token 预算
    └─ python -m kernel.main_kernel key=value ...

configuration/runtime
  kernel/main_kernel.py
    ├─ Hydra: ppo_trainer + kernel_trainer + CLI overrides
    ├─ ray.init
    ├─ TaskRunner.run
    ├─ 加载模型/tokenizer、创建 FSDP/async worker
    ├─ AsyncKernelRewardManager + custom reward function
    └─ RayKernelTrainer.init_workers(); fit()

one fit step
  kernel/kernel_trainer.py
    ├─ dataloader batch + unique uid
    ├─ async vLLM / KernelAgent generate_sequences
    ├─ repeat N × max_turns and union generated responses
    ├─ reward: kernel_async → reward_client → KernelGYM API/GPU workers
    ├─ response/loss mask、filter/rejection/buffer
    ├─ TRLOO: compute_multi_turn_rloo_outcome_advantage
    ├─ update_actor
    │    └─ dp_actor.update_policy → core_algos.compute_policy_loss
    ├─ validate (every TEST_FREQ)
    └─ save checkpoint (every SAVE_FREQ)
```

如果你能够沿着这张图回答下列问题，就已经掌握了该项目 RL 的核心：

1. **哪个组件生成候选？** async vLLM rollout worker。  
2. **哪个组件判断候选是否正确和更快？** KernelGYM API 调度的 GPU worker。  
3. **reward 为什么在最后一个 token？** 它是环境对整段代码结果的 outcome score。  
4. **TRLOO 如何知道“更好”？** 与同一题、同一轮的其他 rollout 的 leave-one-out return 平均比较。  
5. **PPO 如何把比较结果变成权重更新？** 用 advantage 加权 token log-prob ratio，并以 clip 限制策略改变幅度。  
6. **为什么要看筛选与 timeout？** 它们决定真正进入 TRLOO/PPO 的有效样本，而非仅决定表面生成数量。

这正是从 SFT 迁移到项目级 RL 的关键变化：训练目标不再是复述数据中的参考 token，而是通过“采样—执行—反馈—相对比较—受约束更新”的闭环，学习能产出更好 kernel 的生成策略。
