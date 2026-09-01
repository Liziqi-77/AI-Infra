# 第一部分：RL 基础、术语与环境启动

> 返回总目录：[`index.md`](index.md)
>
> 本页为概念教程；启动脚本和调用栈速查见 [`04-runtime-quickstart.md`](04-runtime-quickstart.md)，完整逐行源码解释见 [`appendix/index.md`](appendix/index.md)。

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
| reward | 环境对结果的标量评分 | 每个实际 turn 的最后一个有效 response token 上的 `token_level_scores` |
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



---

**导航**：[总目录](index.md) · [总目录](index.md) · [第二部分](02-launcher-hydra-runtime.md)
