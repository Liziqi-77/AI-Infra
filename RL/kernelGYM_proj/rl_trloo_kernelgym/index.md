# KernelGYM RL 学习文档

这套文档面向已经熟悉 SFT、但刚开始学习 RL 的读者，基于当前 `KernelGYM/drkernel` 源码，解释 TRLOO + PPO 的 Kernel 优化训练闭环。

原来的超大单文件已按“概念学习 → 启动运行 → 逐行源码”拆分。建议先读概念正文，再按需要进入对应源码附录。

## 学习路线

1. [`01-foundations-and-setup.md`](01-foundations-and-setup.md)：SFT 与 RL 的区别、术语、依赖、KernelGYM 服务和 RL 拉起步骤。
2. [`02-launcher-hydra-runtime.md`](02-launcher-hydra-runtime.md)：`8b_trloo_mrs_pr_prs.sh`、`train_rl_common.sh`、Hydra、Ray 和训练器建立。
3. [`03-rollout-reward-training.md`](03-rollout-reward-training.md)：async 多轮 rollout、reward、TRLOO、PPO、筛选、监控和排错。
4. [`04-runtime-quickstart.md`](04-runtime-quickstart.md)：启动 RL 所需脚本、启动顺序和完整调用栈。
5. [`appendix/index.md`](appendix/index.md)：完整项目调用链的逐行源码解释索引。

## 逐行附录路线

```text
Shell launcher
  → Python main_kernel
  → async manager / vLLM engine
  → multi-turn agent
  → reward client / KernelGYM workflow
  → trainer preprocessing / fit
  → TRLOO / PPO core
  → FSDP actor update
```

## 重要说明

- 逐行附录只解释项目自有代码；VERL、vLLM、Ray 和 PyTorch 内部实现属于外部依赖，文档在调用边界处停止。
- 每个附录中的源码行号来自源码文件，不是 Markdown 文件行号。
- 训练脚本的当前源码问题（例如 `FREE_CACHE_ENGINE` 断言冲突、`reference_backend` 配置风险）已在正文和对应附录中标出。

**导航**：从 [`01-foundations-and-setup.md`](01-foundations-and-setup.md) 开始。
