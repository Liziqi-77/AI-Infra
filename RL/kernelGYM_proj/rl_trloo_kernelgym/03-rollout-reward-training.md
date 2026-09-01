# 第三部分：Rollout、Reward、TRLOO 与 PPO

> 返回总目录：[`index.md`](index.md)
>
> 本页为概念教程；完整逐行源码解释见 [`appendix/index.md`](appendix/index.md)。

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
turn 1: prompt → 模型生成第一个 Triton kernel
        ↓
        KernelGYM 返回编译错误、正确性、speedup、profiling 等 feedback
turn 2: 反馈 + 历史 → 模型修正 kernel
        ↓
        再次评测
turn 3: 再次反馈 + 历史 → 模型继续改进
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

这表示每个实际 turn 的 reward 是该 turn 的结果型标量：`_process_single_turn()` 每轮调用 reward，`_postprocess()` 将该 turn 的 score 写入该 turn 最后一个有效 response token；padding turn 的 reward 为 0。之后 return/advantage 再按 turn 和 token mask 传播信号。

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
+ 同一个 turn_indices（都在第 1/2/3 轮）
+ loss_mask 为 1（不是 void/padded turn）
```

理由：第 1 轮没有历史环境反馈，第 3 轮已经看到前两轮反馈；它们处在不同状态分布中，不能直接混成一个 baseline。

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

> **当前源码校正**：`8b_trloo_mrs_pr_prs.sh` 中的 `MODEL_NAME` 含小写 `b`，公共脚本的正则只匹配大写 `B`，所以自动 token 微批预算走 fallback 16384；验证路径写入 `rollout.val_kwargs.n`，而 async 引擎读取 `rollout.val_n_samples`，不能仅凭 `N_VAL=8` 断言验证生成 8 条；多轮实际 turn 编号从 1 开始，补齐行是 -1；每个实际 turn 都会进行环境 reward。


---



---

**导航**：[第二部分](02-launcher-hydra-runtime.md) · [总目录](index.md) · [逐行附录目录](appendix/index.md)
