# Trainer 模块详解

Trainer（训练器）是 rLLM 中负责协调整个 RL 训练过程的核心组件。本文档详细介绍 Trainer 模块的设计、核心类和具体实现。

## 模块结构

```
rllm/trainer/
├── __init__.py                      # 导出 AgentTrainer
├── agent_trainer.py                 # AgentTrainer 统一训练器包装
├── agent_sft_trainer.py             # AgentSFTTrainer 监督微调训练器
├── env_agent_mappings.py            # 环境与 Agent 类映射
├── ray_init_utils.py                # Ray 初始化工具
├── config/                          # YAML 配置文件
│   ├── agent_ppo_trainer.yaml       # PPO 训练默认配置
│   ├── agent_sft_trainer.yaml       # SFT 训练配置
│   ├── tinker_rl_trainer.yaml       # Tinker RL 配置
│   └── tinker_sft_trainer.yaml      # Tinker SFT 配置
├── verl/                            # Verl 后端训练
│   ├── train_agent_ppo.py           # 主入口，TaskRunner
│   ├── agent_ppo_trainer.py         # AgentPPOTrainer
│   ├── agent_workflow_trainer.py    # AgentWorkflowPPOTrainer
│   ├── agent_sdk_trainer.py         # AgentSdkTrainer
│   ├── train_workflow_pipeline.py   # 流水线训练
│   ├── sft_dataset.py               # SFT 数据集
│   └── ray_runtime_env.py           # Ray 运行环境
├── tinker/                          # Tinker 后端训练
│   ├── tinker_launcher.py           # TinkerTrainerLauncher
│   ├── tinker_policy_trainer.py     # 策略训练器
│   ├── tinker_backend.py            # 后端实现
│   └── transform.py                 # 数据转换
└── distill/                         # 蒸馏训练
    ├── advantage.py                 # 优势计算
    └── alignment.py                 # 对齐工具
```

此外还有实验性的统一训练器：

```
rllm/experimental/
├── unified_trainer.py               # UnifiedTrainer 统一训练器
├── buffer.py                        # TrajectoryGroupBuffer
├── sync_coordinator.py              # SyncCoordinator 同步协调器
├── metrics.py                       # MetricsAggregator
├── protocol.py                      # BackendProtocol
└── common/
    ├── advantage.py                 # 优势计算（GRPO 等）
    ├── config.py                    # 配置类
    ├── rejection_sampling.py        # 拒绝采样
    ├── transform.py                 # Episode → TrajectoryGroup 转换
    └── performance.py               # 性能计时工具
```

---

## 1. AgentTrainer - 统一训练器包装

`AgentTrainer` 是用户最常使用的训练器类，它提供了一个简单的接口来启动训练。

### 1.1 类定义

```python
class AgentTrainer:
    """
    包装类，允许用户轻松训练自定义 Agent 和环境，
    无需直接与底层训练基础设施交互。
    
    支持三种后端：
    - 'verl'（默认）：标准训练后端，支持 workflow 和 agent/env 类
    - 'fireworks'：基于流水线的训练后端，针对 workflow 优化
    - 'tinker'：单机训练后端
    """
    
    def __init__(
        self,
        workflow_class: type | None = None,        # 工作流类
        workflow_args: dict | None = None,          # 工作流参数
        agent_class: type | None = None,            # Agent 类
        env_class: type | None = None,              # Environment 类
        agent_args: dict | None = None,             # Agent 参数
        env_args: dict | None = None,               # Environment 参数
        config: dict | list[str] | None = None,     # 配置覆盖
        train_dataset: Dataset | None = None,       # 训练数据集
        val_dataset: Dataset | None = None,         # 验证数据集
        backend: Literal["verl", "fireworks", "tinker"] = "verl",  # 后端
        agent_run_func: Callable | None = None,     # Agent 运行函数
    ):
        ...
    
    def train(self):
        """启动训练"""
        if self.backend == "verl":
            self._train_verl()
        elif self.backend == "fireworks":
            self._train_fireworks()
        elif self.backend == "tinker":
            self._train_tinker()
```

### 1.2 使用方式

**方式一：使用 Workflow 类**

```python
from rllm.trainer import AgentTrainer

trainer = AgentTrainer(
    workflow_class=MyWorkflow,
    workflow_args={"max_steps": 5},
    backend="verl",
    config={"data.train_batch_size": 8},
    train_dataset=dataset,
)
trainer.train()
```

**方式二：使用 Agent + Environment 类**

```python
trainer = AgentTrainer(
    agent_class=MathAgent,
    env_class=SingleTurnEnvironment,
    agent_args={"accumulate_thinking": True},
    env_args={"reward_fn": math_reward_fn},
    backend="verl",
    train_dataset=dataset,
)
trainer.train()
```

**方式三：使用字符串名称**

```python
trainer = AgentTrainer(
    agent_class="math_agent",
    env_class="single_turn",
    backend="verl",
    train_dataset=dataset,
)
trainer.train()
```

### 1.3 后端选择

**Verl 后端**：

```python
def _train_verl(self):
    import ray
    from rllm.trainer.verl.train_agent_ppo import TaskRunner
    
    # 初始化 Ray
    if not ray.is_initialized():
        ray.init(runtime_env=get_ppo_ray_runtime_env())
    
    # 创建远程 TaskRunner
    runner_cls = ray.remote(num_cpus=1)(TaskRunner)
    runner = runner_cls.remote()
    
    # 远程执行训练
    ray.get(runner.run.remote(
        config=self.config,
        workflow_class=self.workflow_class,
        agent_class=self.agent_class,
        env_class=self.env_class,
        ...
    ))
```

**Tinker 后端**：

```python
def _train_tinker(self):
    if self.workflow_class is not None:
        trainer = TinkerWorkflowTrainer(
            config=self.config,
            workflow_class=self.workflow_class,
            train_dataset=self.train_dataset,
        )
    else:
        trainer = TinkerAgentTrainer(
            config=self.config,
            agent_class=self.agent_class,
            env_class=self.env_class,
            train_dataset=self.train_dataset,
        )
    trainer.fit_agent()
```

---

## 2. UnifiedTrainer - 实验性统一训练器

`UnifiedTrainer` 是新一代训练器，采用异步优先设计，支持 on-policy 和 fully-async 训练模式。

### 2.1 类定义

```python
class UnifiedTrainer:
    """后端无关的统一训练器"""
    
    def __init__(
        self,
        backend_cls: type[BackendProtocol],     # 后端类
        config: DictConfig,                      # 配置
        workflow_class: type[Workflow] | None = None,
        train_dataset: Dataset | None = None,
        val_dataset: Dataset | None = None,
        workflow_args: dict | None = None,
        backend_args: dict | None = None,
        traj_grouping_hook: Callable | None = None,
        store: Store | None = None,
        **kwargs,
    ):
        self.workflow_class = workflow_class
        self.workflow_args = workflow_args or {}
        self.store = store
        self.train_dataset = train_dataset
        self.val_dataset = val_dataset
        self.config = config
        self.rllm_config = config.rllm
        self.backend = backend_cls(config=config, **(backend_args or {}))
```

### 2.2 TrainerState

```python
@dataclass
class TrainerState:
    """训练器状态，与后端无关"""
    
    rs_state: RejectionSamplingState = field(default_factory=RejectionSamplingState)
    global_step: int = 0          # 全局步数
    epoch: int = 0                # 当前 epoch
    total_steps: int = 0          # 总步数
    is_training: bool = True      # 是否正在训练
    weight_version: int = 0       # 权重版本
    timing_dict: dict = field(default_factory=dict)
    metrics: dict = field(default_factory=dict)
    extra_info: dict = field(default_factory=dict)
    episodes: list[Episode] | None = None
    trajectory_groups: list[TrajectoryGroup] | None = None
    backend_batch: Any | None = None
    
    def reset_batch(self):
        """重置批次状态"""
        self.rs_state.reset()
        self.episodes = None
        self.trajectory_groups = None
        self.backend_batch = None
        self.timing_dict = {}
        self.metrics = {}
        self.extra_info = {}
```

### 2.3 训练流程

```python
def fit(self):
    """主训练循环"""
    while self.state.is_training:
        # 1. Rollout：收集 Episodes
        self.state.episodes = self._rollout()
        
        # 2. Transform：Episodes → TrajectoryGroups
        self.state.trajectory_groups = transform_episodes_to_trajectory_groups(
            self.state.episodes,
            grouping_hook=self.traj_grouping_hook,
        )
        
        # 3. 拒绝采样（可选）
        if self.config.rejection_sampling.enabled:
            self.state.trajectory_groups = apply_rejection_sampling_and_filtering(
                self.state.trajectory_groups,
                self.state.rs_state,
                self.config.rejection_sampling,
            )
        
        # 4. 计算奖励和优势
        collect_reward_and_advantage_from_trajectory_groups(
            self.state.trajectory_groups,
            algorithm_config=self.config.algorithm,
        )
        
        # 5. 训练更新
        self.backend.update(self.state.trajectory_groups)
        
        # 6. 记录指标
        self._log_metrics()
        
        # 7. 重置批次状态
        self.state.reset_batch()
```

---

## 3. 训练配置

### 3.1 YAML 配置文件

rLLM 使用 YAML 文件来管理训练配置：

```yaml
# agent_ppo_trainer.yaml
data:
  train_batch_size: 8
  val_batch_size: 4
  train_files: "path/to/train/data"
  val_files: "path/to/val/data"

rollout:
  n: 4                    # 每个任务的 rollout 次数
  temperature: 1.0
  top_p: 1.0

algorithm:
  name: "grpo"            # 算法名称
  gamma: 0.0              # 折扣因子
  lam: 1.0                # GAE lambda

rejection_sampling:
  enabled: false
  min_correct: 1
  max_correct: null

agent:
  class: "math_agent"
  args:
    accumulate_thinking: true

env:
  class: "single_turn"
  args:
    reward_fn: "math_reward"

workflow:
  use_workflow: false
```

### 3.2 配置覆盖

可以通过字典或列表覆盖配置：

```python
# 字典方式
config = {"data.train_batch_size": 16, "rollout.n": 8}

# 列表方式
config = ["data.train_batch_size=16", "rollout.n=8"]
```

---

## 4. Verl 后端训练

### 4.1 TaskRunner

`TaskRunner` 是 Verl 后端的远程执行器：

```python
class TaskRunner:
    """Ray 远程 actor，负责分布式 PPO 训练"""
    
    def run(
        self,
        config,
        workflow_class=None,
        workflow_args=None,
        agent_class=None,
        env_class=None,
        agent_args=None,
        env_args=None,
    ):
        # 初始化训练组件
        # 执行训练循环
        # 更新策略
        ...
```

### 4.2 AgentPPOTrainer

`AgentPPOTrainer` 是基于 verl 的 PPO 训练器：

```python
class AgentPPOTrainer:
    """Verl 基础的 PPO 训练器"""
    
    def fit_agent(self):
        # 初始化
        # 训练循环
        # 策略更新
        ...
```

### 4.3 AgentWorkflowPPOTrainer

`AgentWorkflowPPOTrainer` 是用于工作流范式的 PPO 训练器。

---

## 5. Tinker 后端训练

### 5.1 特点

- 单机训练
- 设置简单
- 适合快速原型开发

### 5.2 使用方式

```python
trainer = AgentTrainer(
    workflow_class=MyWorkflow,
    backend="tinker",
    train_dataset=dataset,
)
trainer.train()
```

---

## 6. 优势计算

### 6.1 GRPO 算法

GRPO（Group Relative Policy Optimization）通过比较同一任务的多个 rollout 结果来计算优势：

```python
def collect_reward_and_advantage_from_trajectory_groups(
    trajectory_groups: list[TrajectoryGroup],
    algorithm_config: AlgorithmConfig,
):
    for group in trajectory_groups:
        # 计算组内平均奖励
        rewards = [t.reward for t in group.trajectories]
        mean_reward = np.mean(rewards)
        std_reward = np.std(rewards)
        
        # 计算每个轨迹的优势
        for trajectory in group.trajectories:
            # 标准化优势
            advantage = (trajectory.reward - mean_reward) / (std_reward + 1e-8)
            
            # 将优势分配到每个 Step
            for step in trajectory.steps:
                step.advantage = advantage
```

### 6.2 其他算法

rLLm 还支持其他 RL 算法：
- REINFORCE
- RLOO
- PPO

---

## 7. 拒绝采样

拒绝采样用于过滤低质量的训练数据：

```python
def apply_rejection_sampling_and_filtering(
    trajectory_groups: list[TrajectoryGroup],
    rs_state: RejectionSamplingState,
    config: RejectionSamplingConfig,
):
    filtered_groups = []
    for group in trajectory_groups:
        # 检查是否有正确答案
        has_correct = any(t.reward > 0 for t in group.trajectories)
        
        if has_correct or not config.require_correct:
            filtered_groups.append(group)
    
    return filtered_groups
```

---

## 8. 数据转换

### 8.1 Episode → TrajectoryGroup

```python
def transform_episodes_to_trajectory_groups(
    episodes: list[Episode],
    grouping_hook: Callable = _default_traj_grouping_hook,
) -> list[TrajectoryGroup]:
    """将 Episodes 转换为 TrajectoryGroups"""
    # 按 task_id 分组
    task_groups = defaultdict(list)
    for episode in episodes:
        task_groups[episode.task_id].append(episode)
    
    # 转换为 TrajectoryGroup
    trajectory_groups = []
    for task_id, task_episodes in task_groups.items():
        trajectories = []
        for episode in task_episodes:
            trajectories.extend(episode.trajectories)
        
        trajectory_groups.append(TrajectoryGroup(
            trajectories=trajectories,
            group_id=f"{task_id}:all_groups",
        ))
    
    return trajectory_groups
```

---

## 9. 训练中的指标追踪

### 9.1 MetricsAggregator

```python
class MetricsAggregator:
    """指标聚合器"""
    
    def __init__(self):
        self.metrics = defaultdict(list)
    
    def update(self, metrics: dict):
        for key, value in metrics.items():
            self.metrics[key].append(value)
    
    def get_summary(self) -> dict:
        return {
            key: np.mean(values)
            for key, values in self.metrics.items()
        }
```

### 9.2 常用指标

| 指标 | 说明 |
|------|------|
| `reward/mean` | 平均奖励 |
| `reward/std` | 奖励标准差 |
| `accuracy` | 正确率 |
| `response_length/mean` | 平均响应长度 |
| `kl_divergence` | KL 散度 |

---

## 10. 训练流程总结

```
┌─────────────────────────────────────────────────────────┐
│                      Training Loop                       │
│                                                           │
│  ┌──────────┐    ┌──────────┐    ┌──────────────────┐   │
│  │ Rollout  │───▶│Transform │───▶│  Reject Sampling │   │
│  │(Episodes)│    │          │    │    (Optional)    │   │
│  └──────────┘    └────┬─────┘    └────────┬─────────┘   │
│                       │                   │             │
│                       └────────┬──────────┘             │
│                                ▼                         │
│                       ┌──────────────┐                  │
│                       │ Advantage    │                  │
│                       │ Computation  │                  │
│                       └──────┬───────┘                  │
│                              │                          │
│                              ▼                          │
│                       ┌──────────────┐                  │
│                       │ Policy       │                  │
│                       │ Update       │                  │
│                       └──────┬───────┘                  │
│                              │                          │
│                              ▼                          │
│                       ┌──────────────┐                  │
│                       │ Log Metrics  │                  │
│                       └──────────────┘                  │
└─────────────────────────────────────────────────────────┘
```

---

## 11. 总结

| 类 | 用途 | 特点 |
|----|------|------|
| `AgentTrainer` | 统一训练器包装 | 简单易用，支持多种后端 |
| `UnifiedTrainer` | 实验性统一训练器 | 异步优先，支持 fully-async |
| `TaskRunner` | Verl 远程执行器 | Ray 分布式 |
| `AgentPPOTrainer` | Verl PPO 训练器 | 标准 PPO 训练 |
| `TrainerState` | 训练器状态 | 记录训练进度和指标 |

理解 Trainer 模块后，建议继续学习 Tools 和 Rewards 模块。
