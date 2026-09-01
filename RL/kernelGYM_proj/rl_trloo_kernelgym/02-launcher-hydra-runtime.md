# 第二部分：RL 启动器、Hydra 与运行时

> 返回总目录：[`index.md`](index.md)
>
> 本页为概念教程；完整逐行源码解释见 [`appendix/index.md`](appendix/index.md)。

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
| `N_VAL=8` | launcher 将 `N_VAL=8` 写入 `rollout.val_kwargs.n`；但当前 async `generate_sequences()` 实际读取的是 `rollout.val_n_samples`，因此该值可能不会控制真实验证候选数，必须以 resolved config 和引擎代码确认。|
| 两个 `*_OFFLOAD=True` | FSDP 将 actor 参数/优化器状态按配置卸载以省显存；常以传输/吞吐换显存。|
| `LEARNING_RATE=1e-6` | actor optimizer 学习率。|
| `TRAIN_BATCH_SIZE=16` | 一次训练 iteration 的目标 prompt 数；与 rollout 数相乘后会膨胀。|
| `PPO_MINI_BATCH_SIZE=16` | actor update 时的大 batch 再切 mini-batch 的初始样本数；`RayKernelTrainer.__init__()` 会在多轮模式按 `max_turns` 放大，FSDP worker 初始化时还会结合 rollout `n`、world size 和 sequence parallel 做归一化，因此最终 worker 内值不一定仍是 16。|
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
6. `FREE_CACHE_ENGINE=False`：**当前源码存在启动冲突**。`AsyncLLMEngineManager.__init__()` 和 `generate_sequences()` 都断言 `free_cache_engine=True`（`async_server.py:158–175`）；因此原样值为 `False` 时，async rollout manager 可能在初始化阶段直接失败。运行前应改为 `True`，或先修复源码/配置。
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

`generate_model_micro_token()` 会用正则 `([0-9]+)B` 从模型名提取规模：7B→8192，14B→4096，32B→2048。当前 launcher 的 `MODEL_NAME=hkust-nlp/drkernel-8b` 使用小写 `b`，不匹配该正则，因此实际走默认回退值 16384。之后检查：

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

> **启动前必须检查的源码冲突**：当前 `8b_trloo_mrs_pr_prs.sh` 设置 `FREE_CACHE_ENGINE=False`，但 `AsyncLLMEngineManager.__init__()` 在 `async_server.py:158` 断言 `free_cache_engine=True`。因此不能把原始命令描述成一定可直接运行；在实际运行前将该值改为 `True`，或按你的分支修改断言和 sleep/wake-up 逻辑。另一个高风险点是 `kernel/rewards/kernel_reward.py:124–129` 无默认读取 `reward_config.reference_backend`，而当前 `kernel_trainer.yaml` 没有该字段；异常会在 `compute_kernel_reward_batch()` 外层被捕获并转成 penalty。

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



---

**导航**：[第一部分](01-foundations-and-setup.md) · [总目录](index.md) · [第三部分](03-rollout-reward-training.md)
