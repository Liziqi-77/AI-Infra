# RL 启动脚本与完整调用栈

> 返回总目录：[`index.md`](index.md)  
> 基础教程：[`01-foundations-and-setup.md`](01-foundations-and-setup.md) · [`02-launcher-hydra-runtime.md`](02-launcher-hydra-runtime.md) · [`03-rollout-reward-training.md`](03-rollout-reward-training.md)  
> 逐行源码：[`appendix/index.md`](appendix/index.md)
>
> 本页是运行速查；完整概念解释见 [`03-rollout-reward-training.md`](03-rollout-reward-training.md)。

本文只回答两个问题：

1. 启动 KernelGYM RL 需要运行哪些脚本？
2. 这些脚本启动后，代码的调用栈是什么？

---

## 1. 先区分三类脚本

启动 RL 时容易把“安装依赖”“启动评测环境”和“启动模型训练”混为一件事。它们实际上是三类不同脚本。

| 类别 | 脚本 | 运行频率 | 作用 |
|---|---|---:|---|
| 一次性安装 | `/home/l00899543/RL/KernelGYM/setup.sh` | 每个环境一次 | 安装 KernelGYM 服务端依赖、Redis、`iproute2` |
| 一次性安装 | `/home/l00899543/RL/KernelGYM/drkernel/setup.sh` | 每个环境一次 | 初始化 VERL，安装 Ray/vLLM/Torch/FlashAttention 等 RL 依赖 |
| 评测环境启动 | `/home/l00899543/RL/KernelGYM/start_all_with_monitor.sh` | 每次训练前 | 启动 Redis、API server、worker monitor、GPU workers |
| 多节点评测环境启动 | `/home/l00899543/RL/KernelGYM/start_worker_multinode.sh` | 每个远程 worker 节点 | 将远程 worker 节点接入 API/Redis 所在节点 |
| RL 训练启动 | `drkernel/kernel/scripts/rl/8b_trloo_mrs_pr_prs.sh` | 每次 8B RL 训练 | 设置 recipe 并启动 `kernel.main_kernel` |
| RL 训练启动 | `drkernel/kernel/scripts/rl/14b_trloo_mrs_pr_prs.sh` | 每次 14B RL 训练 | 14B 版本的同类 recipe |

### 最小必需集合

单机或评测服务已准备好的情况下，每次真正运行 RL 至少需要：

```text
start_all_with_monitor.sh       # KernelGYM 评测环境
8b_trloo_mrs_pr_prs.sh          # RL 训练
```

第一次安装还需要：

```text
setup.sh                        # 根项目依赖
 drkernel/setup.sh              # RL/VERL 依赖
```

`SFT` cold start 脚本是推荐的模型预热步骤，不是 RL 运行时必须拉起的脚本：

```text
drkernel/kernel/scripts/sft/8b-coldstart.sh
```

如果你已经有可用的 SFT checkpoint，也可以直接把它作为 RL 的 `MODEL_PATH`。

---

## 2. 单机启动顺序

假设 KernelGYM API、Redis、评测 GPU worker 和 RL 训练都在同一台机器上。

### 2.1 第一次安装：只做一次

终端 1：

```bash
cd /home/l00899543/RL/KernelGYM
bash setup.sh
```

对应根目录 `setup.sh:6–9`：

1. `pip install -r requirements.txt --user` 安装服务端 Python 依赖；
2. `pip install pydantic-settings --user` 安装配置依赖；
3. `sudo apt update` 更新系统包索引；
4. `sudo apt-get install iproute2 redis -y` 安装网络工具和 Redis。

终端 2：

```bash
cd /home/l00899543/RL/KernelGYM/drkernel
bash setup.sh
```

对应 `drkernel/setup.sh:7–34`：

1. `git submodule update --init` 初始化 VERL submodule；
2. 在 `verl` 目录 editable install；
3. 安装固定版本的 Ray、vLLM、Torch、Transformers 等；
4. 下载并安装 FlashAttention wheel；
5. 安装 W&B、sandbox-fusion 等辅助依赖。

这两个脚本不是每次训练都要重复执行；只有切换了 Python 环境、重新部署机器或依赖发生变化时才需要重跑。

### 2.2 每次训练前：启动 KernelGYM 评测环境

终端 1：

```bash
cd /home/l00899543/RL/KernelGYM
./start_all_with_monitor.sh
```

脚本内部启动以下进程（`start_all_with_monitor.sh:94–176`）：

```text
Redis
  ├─ python -m kernelgym.server.api.server
  ├─ python -m kernelgym.worker.worker_monitor --persistent
  └─ python -m kernelgym.worker.single_worker --worker-id ... --device cuda:N --persistent
```

启动后检查：

```bash
curl "http://<kernelgym-host>:<api-port>/health"
curl "http://<kernelgym-host>:<api-port>/workers/status"
```

第一个 endpoint 用于确认 API 健康，第二个用于确认 GPU worker 已注册。训练入口还会自行检查 `<KERNELGYM_SERVER_URL>/health`，见 `drkernel/kernel/main_kernel.py:56–88,246–252`。

### 2.3 每次训练前：设置训练变量

训练端至少需要知道评测服务地址、模型、数据和 checkpoint 输出位置：

```bash
export KERNELGYM_SERVER_URL="http://<kernelgym-host>:<api-port>"
export HDFS_DATA_PATH="/absolute/path/to/rl-parquet"
export HDFS_CHECKPOINT_PATH="/absolute/path/to/checkpoints"
export PROJECT_NAME="kernelgym-rl"
```

然后确认 `8b_trloo_mrs_pr_prs.sh:3–7` 中的：

```bash
TRAIN_DATASET=("hkust-nlp/drkernel-rl-data")
VALID_DATASET=("hkust-nlp/drkernel-validation-data")
MODEL_NAME=hkust-nlp/drkernel-8b
MODEL_PATH=${MODEL_NAME}
```

注意：当前脚本中的 dataset 名会被解析为：

```text
${HDFS_DATA_PATH}/hkust-nlp/drkernel-rl-data.parquet
${HDFS_DATA_PATH}/hkust-nlp/drkernel-validation-data.parquet
```

它不是在这里自动下载 Hugging Face dataset。

### 2.4 每次训练：启动 RL launcher

终端 2：

```bash
cd /home/l00899543/RL/KernelGYM/drkernel/kernel/scripts/rl
bash 8b_trloo_mrs_pr_prs.sh
```

或者先进行只验证模式：

```bash
bash 8b_trloo_mrs_pr_prs.sh --val_only True
```

学习型小实验可以缩小规模：

```bash
bash 8b_trloo_mrs_pr_prs.sh \
  --train_batch_size 2 \
  --rollout_n 2 \
  --total_epochs 1 \
  --max_turn 2
```

### 2.5 当前源码的启动前阻塞项

原始 8B recipe **不能无条件视为开箱即用**，运行前检查：

1. `8b_trloo_mrs_pr_prs.sh:82` 设置 `FREE_CACHE_ENGINE=False`，但 `kernel/workers/rollout/async_server.py:158` 对 `free_cache_engine` 有 `assert True`，async manager 初始化可能直接失败。运行前建议先改为 `True`，或按你的分支修复 manager 的 sleep/wake-up 逻辑。
2. `kernel/rewards/kernel_reward.py:124–129` 使用 `getattr(reward_config, "reference_backend")`，当前 `kernel/config/kernel_trainer.yaml` 可能没有这个字段；异常在 `kernel_reward.py:191–203` 被转换为 penalty，可能表现为 reward 全部异常/惩罚。
3. `N_VAL=8` 传给 `rollout.val_kwargs.n`，但当前 async engine 在 `vllm_async_engine.py:2144` 读取 `rollout.val_n_samples`；验证候选数量必须看最终 resolved config。

---

## 3. 远程/多节点评测启动

当 RL 训练节点不负责运行 KernelGYM GPU worker 时，进程分布如下：

```text
评测主节点
  ├─ Redis
  ├─ KernelGYM API server
  └─ worker monitor

评测 worker 节点 1..N
  └─ start_worker_multinode.sh
       └─ start_worker_node.sh
            └─ kernelgym.worker.single_worker --device cuda:N

训练节点
  └─ 8b_trloo_mrs_pr_prs.sh
```

在评测主节点启动 API/Redis，在每个评测 worker 节点运行：

```bash
cd /home/l00899543/RL/KernelGYM
./start_worker_multinode.sh
```

然后在训练节点设置：

```bash
export KERNELGYM_SERVER_URL="http://<evaluation-api-host>:<api-port>"
```

多节点 worker 需要确认 `API_HOST`、`API_PORT`、`REDIS_HOST`、`REDIS_PORT` 等环境变量，具体入口见 `start_worker_multinode.sh` 和 `start_worker_node.sh`。训练端不需要在本地再启动一套 API，但必须能访问该 URL。

---

## 4. 启动后的完整调用栈

下面按时间顺序分别说明“服务端栈”和“训练端栈”。它们在 reward 请求处汇合。

### 4.1 服务端：评测环境栈

```text
start_all_with_monitor.sh
  ├─ redis-server
  ├─ python -m kernelgym.server.api.server
  │    └─ FastAPI app
  │         ├─ POST /evaluate
  │         ├─ GET  /status/{task_id}
  │         ├─ GET  /results/{task_id}
  │         └─ GET  /health
  ├─ python -m kernelgym.worker.worker_monitor --persistent
  └─ python -m kernelgym.worker.single_worker --device cuda:N
       └─ GPU worker / subprocess pool / toolkit
```

一次 `/evaluate` 请求的服务端调用栈：

```text
POST /evaluate
  └─ kernelgym.server.api.server.evaluate_kernel()
       └─ _execute_workflow()
            ├─ get_task_result(task_id)       # 命中缓存时提前返回
            ├─ get_workflow_controller("kernelbench")
            └─ KernelBenchWorkflowController.handle_request()
                 ├─ EvaluationTask.from_dict(payload)
                 ├─ _validate_inputs()
                 ├─ _create_paired_tasks()
                 │    ├─ kernel evaluation task
                 │    └─ reference timing task
                 ├─ scheduler.submit(kernel_task_spec)
                 ├─ scheduler.wait(kernel_task_id)
                 ├─ scheduler.submit(ref_task_spec)
                 ├─ scheduler.wait(ref_task_id)
                 ├─ _combine_results(reference_result, kernel_result)
                 └─ task_mgr.complete_task(task_id, result)
```

训练端的 client 随后：

```text
GET /status/{task_id}
  └─ task_mgr.get_task_status(task_id)

GET /results/{task_id}
  └─ task_mgr.get_task_result(task_id)
```

对应代码：

- API endpoint：`kernelgym/server/api/server.py:435–461,592–640,673–685`；
- workflow 调度：`kernelgym/server/api/server.py:325–358`；
- KernelBench workflow：`kernelgym/workflow/kernelbench.py:31–182`；
- 任务配对/结果合并：`kernelgym/workflow/kernelbench_helpers.py:44–127`。

### 4.2 训练端：从 shell 到 Python

```text
bash 8b_trloo_mrs_pr_prs.sh
  ├─ 设置 recipe 变量
  ├─ source train_rl_common.sh
  │    ├─ source ../../../setup_env.sh
  │    ├─ 顶层设置默认变量
  │    └─ 定义 parse_arguments/setup_training_environment/run_training
  └─ main "$@"
       ├─ parse_arguments "$@"
       ├─ setup_training_environment
       │    ├─ 解析模型路径
       │    ├─ 解析 checkpoint 目录
       │    ├─ 解析 GPU/节点数
       │    ├─ 计算 PPO micro-token
       │    ├─ 拆解 clip ratio
       │    └─ 格式化 parquet 路径
       └─ run_training
            └─ PYTHONUNBUFFERED=1 python -m kernel.main_kernel key=value ...
```

### 4.3 Python 入口到 trainer

```text
kernel.main_kernel.main(config)
  └─ run_ppo(config)
       ├─ 保存 CUDA_VISIBLE_DEVICES 环境
       ├─ ray.init(...)
       ├─ TaskRunner.remote()
       └─ ray.get(runner.run.remote(config))
            └─ TaskRunner.run(config)
                 ├─ OmegaConf.resolve(config)
                 ├─ copy_to_local(model.path)
                 ├─ hf_tokenizer(model)
                 ├─ hf_processor(model)
                 ├─ 选择 FSDP + AsyncActorRolloutRefWorker
                 ├─ 创建 Ray role/resource pool
                 ├─ 选择 AsyncKernelRewardManager
                 ├─ 动态加载 compute_kernel_reward_batch
                 ├─ health check KernelGYM
                 ├─ 创建 train/validation reward function
                 ├─ RayKernelTrainer(...)
                 ├─ trainer.init_workers()
                 └─ trainer.fit()
```

对应 `drkernel/kernel/main_kernel.py:90–300`。

### 4.4 trainer 初始化阶段

```text
RayKernelTrainer.__init__
  ├─ 多轮时调整 actor PPO mini-batch
  ├─ 兼容外部 advantage estimator=trloo
  └─ _initialize_batch_filter()

RayKernelTrainer.init_workers()
  ├─ resource_pool_manager.create_resource_pool()
  ├─ 创建 actor_rollout WorkerGroup
  ├─ actor_rollout_wg.init_model()
  │    ├─ FSDP actor model
  │    ├─ CodeDataParallelPPOActor
  │    └─ RolloutWorker
  ├─ async_rollout_mode=True
  └─ AsyncLLMEngineManager(...)
       ├─ 选择 MultiTurnAsyncvLLMEngine
       ├─ 创建 Ray async engine actors
       ├─ init_engine()
       └─ sleep()
```

注意：`AsyncActorRolloutRefWorker.generate_sequences()` 本身明确抛出 `NotImplementedError`（`fsdp_workers.py:1639–1641`）。async 模式不是走这个同步 dispatch 方法，而是由 `AsyncLLMEngineManager` 把请求分发到独立的 async engine actor。

### 4.5 一个 training step

```text
RayKernelTrainer.fit()
  ├─ 从 train_dataloader 取 TRAIN_BATCH_SIZE 个原始 prompt
  ├─ 为样本生成 uid
  ├─ async_rollout_manager.wake_up()
  ├─ AsyncLLMEngineManager.generate_sequences()
  │    ├─ prompts.chunk(number_of_async_engines)
  │    ├─ worker.generate_sequences.remote(chunk)
  │    └─ DataProto.concat(outputs)
  ├─ async_rollout_manager.sleep()
  ├─ 按 ROLLOUT_N × MAX_TURN 复制任务元数据
  ├─ 合并 responses / logprobs / turn_indices
  ├─ 构造 response_mask / loss_mask
  ├─ 进行 rollout correction / coverage filter / rejection filter
  ├─ 把 token_level_scores 转为 token_level_rewards
  ├─ compute_multi_turn_advantage(..., adv_estimator="trloo")
  ├─ critic update（若启用）
  ├─ actor_rollout_wg.update_actor(batch)
  ├─ validation（达到 TEST_FREQ）
  ├─ checkpoint（达到 SAVE_FREQ）
  └─ logger.log(metrics)
```

### 4.6 async engine 内部

```text
MultiTurnAsyncvLLMEngine.generate_sequences()
  ├─ 训练：prompts.repeat(rollout.n, interleave=True)
  ├─ 验证：prompts.repeat(val_n_samples, interleave=True)
  ├─ 组装 sampling_params
  ├─ 为每个 prompt 创建 asyncio task
  └─ asyncio.gather(*tasks)
       └─ _async_agent_loop()
            ├─ create_agent("KernelAgent")
            ├─ create_environment(...)
            ├─ env.reset(extra_info)
            ├─ while not done and turn < max_turns
            │    └─ _process_single_turn()
            │         ├─ 组织当前轮 messages
            │         ├─ tokenizer.apply_chat_template()
            │         ├─ engine.generate()
            │         ├─ 取得 response/token_ids/logprobs
            │         ├─ agent.generate_thought_and_action()
            │         ├─ reward_fn(...)
            │         ├─ 保存 turn reward / speedup / correctness
            │         └─ 返回 tool feedback
            ├─ agent.finalize()
            └─ _postprocess()
                 ├─ padding prompt/response/logprobs
                 ├─ turn index：实际轮次 1/2/3，padding=-1
                 ├─ 生成 response_mask/loss_mask
                 ├─ 每个实际 turn 的最后有效 token 写入 reward
                 └─ 返回 DataProto
```

### 4.7 reward 汇合点

```text
reward_fn(...)
  └─ AsyncKernelRewardManager.__call__()
       └─ execute_env()
            └─ compute_kernel_reward_batch()
                 ├─ extract_kernel_code()
                 ├─ 组装 reference/kernel/entry_point/uuid
                 └─ KernelRewardClient.compute_batch_rewards()
                      ├─ _preflight_validate()
                      ├─ _HybridHttpWorker.submit_and_poll()
                      │    ├─ token bucket acquire
                      │    ├─ POST /evaluate
                      │    ├─ GET /status/{task_id}
                      │    └─ GET /results/{task_id}
                      ├─ _get_reward_func()
                      ├─ calculate_reward_speedup()
                      └─ 按原始顺序返回 reward result
```

结果回到 rollout：

```text
reward result
  └─ reward_tensor
       └─ MultiTurnAsyncvLLMEngine._postprocess()
            └─ batch.token_level_scores
                 └─ trainer.compute_multi_turn_advantage()
```

### 4.8 TRLOO 与 PPO 更新

```text
batch.token_level_rewards
  └─ compute_multi_turn_advantage()
       └─ compute_multi_turn_rloo_outcome_advantage()
            ├─ scores = token_level_rewards.sum(dim=-1)
            ├─ returns = multi-turn discounted returns
            ├─ 按 (uid, turn_indices) 分组
            ├─ 计算 leave-one-out baseline
            ├─ advantage = return - baseline
            └─ 广播到 response token

advantages + old_log_probs + responses
  └─ update_actor(batch)
       └─ AsyncActorRolloutRefWorker.update_actor()
            └─ CodeDataParallelPPOActor.update_policy(data)
                 └─ `verl_patch/workers/code/actor/dp_actor.py:update_policy`
                 ├─ split PPO mini-batch
                 ├─ split token-limited micro-batch
                 ├─ 当前 actor forward → log_prob/entropy
                 ├─ compute_policy_loss()
                 │    ├─ ratio = exp(log_prob - old_log_prob)
                 │    ├─ PPO clip
                 │    └─ dual clip
                 ├─ entropy/KL 项（本 recipe 系数为 0）
                 ├─ loss.backward()
                 ├─ _optimizer_step()
                 └─ 返回 actor metrics/grad norm
```

---

## 5. 一句话记忆

启动 RL 的运行顺序是：

```text
安装依赖（只需一次）
  → start_all_with_monitor.sh（启动 KernelGYM 环境）
  → 检查 /health 和 /workers/status
  → 设置 KERNELGYM_SERVER_URL、模型、数据、checkpoint
  → 8b_trloo_mrs_pr_prs.sh（启动 RL 训练）
  → shell common layer
  → kernel.main_kernel
  → RayKernelTrainer.fit
  → async vLLM 多轮 rollout
  → KernelGYM reward
  → TRLOO advantage
  → PPO actor update
  → validation/checkpoint
```

也就是说，`start_all_with_monitor.sh` 提供 RL 的“环境”，`8b_trloo_mrs_pr_prs.sh` 提供 RL 的“训练循环入口”；两者缺一不可。
