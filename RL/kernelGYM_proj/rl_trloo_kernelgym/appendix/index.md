# KernelGYM RL 逐行源码附录

> 返回总目录：[`../index.md`](../index.md)

这组附录按实际调用链拆分，每个文件保留项目源码行、语法解释、作用说明和当前执行状态。第三方 VERL、vLLM、Ray 的内部源码不展开；进入这些依赖的项目边界行会说明输入、输出和副作用。

## 阅读顺序

1. [`Shell 入口：setup_env、8B launcher 与公共启动器`](01-shell-launchers.md)
2. [`Python 主入口：main_kernel.py`](02-python-entry.md)
3. [`Async manager 与 vLLM engine 初始化`](03-async-bootstrap.md)
4. [`多轮 rollout：生成、agent loop 与 postprocess`](04-multiturn-rollout.md)
5. [`Reward pipeline：manager、代码抽取与 HTTP client`](05-reward-pipeline.md)
6. [`Trainer 基础：mask、advantage、dataloader 与 worker 初始化`](06-trainer-foundation.md)
7. [`Trainer 预处理：采样器、batch 与过滤准备`](07-trainer-preprocessing.md)
8. [`Trainer fit：奖励、TRLOO、actor、验证与 checkpoint`](08-trainer-fit-update.md)
9. [`TRLOO 与 PPO core algorithms`](09-trloo-ppo-core.md)
10. [`Actor policy update：PPO loss 与 optimizer`](10-actor-update.md)
11. [`FSDP worker：模型、rollout 与 actor update 边界`](11-fsdp-boundaries.md)
12. [`Agent 与 environment：KernelAgent、环境工厂与 CodeSandboxEnv`](12-agent-environment.md)
13. [`KernelGYM API 与 KernelBench workflow`](13-kernelgym-workflow.md)

## 概念章节与附录对应

| 想理解的内容 | 先读概念章节 | 再读逐行附录 |
|---|---|---|
| RL 与 SFT、环境和启动步骤 | [`01-foundations-and-setup.md`](../01-foundations-and-setup.md) | `01-shell-launchers.md` |
| launcher、Hydra、Ray 运行时 | [`02-launcher-hydra-runtime.md`](../02-launcher-hydra-runtime.md) | `01-shell-launchers.md`、`02-python-entry.md`、`03-async-bootstrap.md` |
| rollout 与多轮交互 | [`03-rollout-reward-training.md`](../03-rollout-reward-training.md) | `03-async-bootstrap.md`、`04-multiturn-rollout.md`、`12-agent-environment.md` |
| reward 与 KernelGYM 评测 | [`03-rollout-reward-training.md`](../03-rollout-reward-training.md) | `05-reward-pipeline.md`、`13-kernelgym-workflow.md` |
| trainer、TRLOO、PPO 更新 | [`03-rollout-reward-training.md`](../03-rollout-reward-training.md) | `06`–`11` 附录 |

## 覆盖规则

- 源码行号指向当前 checkout 的源文件，不是拆分后文档的行号。
- 注释和空行也保留；空行说明为无运行时效果，注释说明为不执行。
- 当前源码中重复生成的索引段只保留一份 canonical 解释；不会因为文档拆分再复制一份。
- 如果源码发生变化，必须重新核对附录中的真实源码行号和行覆盖范围。

**导航**：[`../03-rollout-reward-training.md`](../03-rollout-reward-training.md) · [总目录](../index.md) · [`01-shell-launchers.md`](01-shell-launchers.md)
