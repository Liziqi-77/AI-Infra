# Workflow 模块详解

Workflow（工作流）是 rLLM 中用于编排 Agent 和环境交互的高级抽象。它定义了完整的交互流程、终止条件、奖励计算和轨迹收集逻辑。

本文档详细介绍 Workflow 模块的设计、核心类和具体实现。

## 模块结构

```
rllm/workflows/
├── __init__.py                  # 导出 Workflow, TerminationReason, TerminationEvent
├── workflow.py                  # Workflow 抽象基类
├── simple_workflow.py           # SimpleWorkflow 单步工作流
├── single_turn_workflow.py      # SingleTurnWorkflow 单轮工作流
├── multi_turn_workflow.py       # MultiTurnWorkflow 多轮工作流
├── cumulative_workflow.py       # CumulativeWorkflow 累积工作流
├── distillation_workflow.py     # DistillationWorkflow 蒸馏工作流
├── eval_protocol_workflow.py    # Eval protocol 工作流
├── store.py                     # 跨 Episode 共享存储
└── timing_mixin.py              # 计时追踪 Mixin
```

---

## 1. Workflow 抽象基类

`Workflow` 是所有工作流的抽象基类，定义了工作流的标准接口和通用功能。

### 1.1 类定义

```python
class Workflow(ABC):
    def __init__(
        self,
        rollout_engine: RolloutEngine,     # 推理引擎
        executor: ThreadPoolExecutor,       # 线程池执行器
        timeout=1e6,                        # 超时时间
        gamma=0.0,                          # 折扣因子
        reward_bonus_coeff=0.0,             # 奖励整形系数
        store: Store | None = None,         # 跨 Episode 共享存储
        **kwargs,
    ):
        self.rollout_engine = rollout_engine
        self.executor = executor
        self.timeout = int(timeout)
        self.gamma = gamma
        self.reward_bonus_coeff = reward_bonus_coeff
        self.store = store
        self._completed_trajectories: list[Trajectory] = []
```

### 1.2 核心方法

#### `run(task, uid, **kwargs) -> Episode | None`

**抽象方法**：子类必须实现此方法来定义工作流的核心逻辑。

```python
@abstractmethod
async def run(self, task: dict, uid: str, **kwargs) -> Episode | None:
    """
    在单个任务上执行工作流
    
    Args:
        task: 要执行的任务
        uid: 任务的唯一标识符
        **kwargs: 额外的关键字参数
    
    Returns:
        Episode: 工作流产生的回合
    """
    pass
```

#### `run_with_termination_handling(task, uid, **kwargs) -> Episode`

**作用**：`run()` 的包装方法，处理终止事件、错误和超时。

```python
async def run_with_termination_handling(self, task, uid, **kwargs) -> Episode:
    timeout = kwargs.pop("timeout", self.timeout)
    
    try:
        coro = self.run(task, uid, **kwargs)
        output = await asyncio.wait_for(coro, timeout=timeout)
        if output is not None and isinstance(output, Episode):
            return output  # 已经后处理
        return self.postprocess_episode(self.collect_trajectories(), TerminationReason.UNKNOWN)
    except asyncio.TimeoutError:
        return self.postprocess_episode(self.collect_trajectories(), TerminationReason.TIMEOUT)
    except TerminationEvent as e:
        return self.postprocess_episode(self.collect_trajectories(), e.reason)
    except Exception as e:
        error_details = {
            "error_message": str(e),
            "error_type": type(e).__name__,
            "traceback": traceback.format_exc(),
        }
        return self.postprocess_episode(self.collect_trajectories(), TerminationReason.ERROR, error=error_details)
```

**处理的异常类型**：

| 异常 | 终止原因 |
|------|---------|
| `asyncio.TimeoutError` | `TIMEOUT` |
| `TerminationEvent` | 事件中的原因 |
| 其他 `Exception` | `ERROR` |

#### `commit(name, agent, trajectory, reset)`

**作用**：提交一个轨迹用于训练。

```python
def commit(self, name=None, agent=None, trajectory=None, reset=False):
    """
    提交轨迹
    
    Args:
        name: 轨迹名称
        agent: 生成轨迹的 Agent
        trajectory: 要提交的轨迹
        reset: 是否重置 Agent
    """
    assert agent is not None or trajectory is not None
    assert agent is None or trajectory is None
    
    traj = agent.trajectory if agent is not None else trajectory
    if name:
        traj.name = name
    if traj.steps:
        self._completed_trajectories.append(deepcopy(traj))
    
    if agent is not None and reset:
        agent.reset()
```

#### `collect_trajectories() -> Episode`

**作用**：收集工作流中的所有轨迹。

```python
def collect_trajectories(self) -> Episode:
    episode = Episode()
    
    # 首先添加已提交的轨迹
    episode.trajectories.extend(self._completed_trajectories)
    
    # 跟踪已提交的轨迹 UID
    completed_trajectory_uids = {t.uid for t in self._completed_trajectories}
    
    # 添加 Agent 中尚未提交的轨迹
    for attr_name in dir(self):
        if attr_name.startswith("_"):
            continue
        attr_value = getattr(self, attr_name)
        if (
            isinstance(attr_value, BaseAgent)
            and hasattr(attr_value, "trajectory")
            and attr_value.trajectory.uid not in completed_trajectory_uids
            and len(attr_value.trajectory.steps) > 0
        ):
            episode.trajectories.append(deepcopy(attr_value.trajectory))
    
    return episode
```

#### `postprocess_episode(episode, termination_reason, error)`

**作用**：后处理 Episode，包括分配任务 ID、计算奖励、设置正确性标志等。

```python
def postprocess_episode(self, episode, termination_reason=None, error=None) -> Episode:
    # 1. 分配任务 ID 和任务
    episode.id = self.uid
    episode.task = self.task
    
    for trajectory in episode.trajectories:
        # 清理空步骤
        if trajectory.steps and not trajectory.steps[-1].chat_completions:
            trajectory.steps.pop()
        
        # 2. 计算轨迹级别奖励
        self.compute_trajectory_reward(trajectory)
        
        # 3. 调整步骤级别奖励（奖励整形或折扣）
        if len(trajectory.steps) > 1:
            self.adjust_step_rewards(trajectory)
    
    # 4. 分配 Episode 级别正确性标志
    self.assign_episode_correctness(episode)
    
    # 5. 收集额外指标
    self.collect_metrics(episode)
    
    # 6. 存储错误详情
    if error is not None:
        episode.info["error"] = error
    
    # 7. 分配终止原因
    episode.termination_reason = termination_reason or TerminationReason.UNKNOWN
    
    return episode
```

### 1.3 奖励计算

#### `compute_trajectory_reward(trajectory)`

**作用**：计算轨迹级别的奖励（默认是所有 step.reward 的和）。

```python
def compute_trajectory_reward(self, trajectory: Trajectory) -> None:
    trajectory.reward = np.sum([d.reward for d in trajectory.steps])
```

#### `adjust_step_rewards(trajectory)`

**作用**：调整步骤级别的奖励，支持奖励整形和折扣。

```python
def adjust_step_rewards(self, trajectory: Trajectory) -> None:
    # 奖励整形
    # s[i].reward = s[i].reward + bonus * (s[i].reward - s[i-1].reward) for i > 0
    if self.reward_bonus_coeff > 0.0:
        raw_rewards = [step.reward for step in trajectory.steps]
        for i in range(1, len(trajectory.steps)):
            trajectory.steps[i].reward += self.reward_bonus_coeff * (raw_rewards[i] - raw_rewards[i - 1])
    
    # 计算 Monte Carlo 回报（反向迭代）
    # G_t = R_{t+1} + γ * R_{t+2} + γ² * R_{t+3} + ... + γ^{T-t-1} * R_T
    if self.gamma > 0.0:
        G = 0.0
        for step in reversed(trajectory.steps):
            G = step.reward + self.gamma * G
            step.reward = G  # 用 MC 回报替换奖励
```

#### `assign_episode_correctness(episode)`

**作用**：分配 Episode 级别的正确性标志。

```python
def assign_episode_correctness(self, episode: Episode) -> None:
    total_reward = 0
    for trajectory in episode.trajectories:
        total_reward += trajectory.reward or 0
    episode.is_correct = total_reward > 0
```

### 1.4 重置和线程安全

#### `reset(task, uid)`

**作用**：重置工作流状态。

```python
def reset(self, task: dict | None = None, uid: str | None = None) -> None:
    self.uid = uid
    self.task = task
    self._completed_trajectories = []
    
    # 重置所有 Agent
    for attr_name in dir(self):
        if attr_name.startswith("_"):
            continue
        attr_value = getattr(self, attr_name)
        if isinstance(attr_value, BaseAgent) and hasattr(attr_value, "reset"):
            attr_value.reset()
            attr_value.trajectory.task = task
    
    # 重置所有 Environment
    for attr_name in dir(self):
        if attr_name.startswith("_"):
            continue
        attr_value = getattr(self, attr_name)
        if isinstance(attr_value, BaseEnv) and hasattr(attr_value, "reset"):
            attr_value.reset(task=task)
```

#### `is_multithread_safe() -> bool`

**作用**：检查工作流是否线程安全。

```python
def is_multithread_safe(self) -> bool:
    for attr_name in dir(self):
        attr_value = getattr(self, attr_name)
        if isinstance(attr_value, BaseEnv) and not attr_value.is_multithread_safe():
            return False
    return True
```

#### `run_in_executor(fn, *args, **kwargs)`

**作用**：在线程池中运行函数。

```python
async def run_in_executor(self, fn, *args, **kwargs):
    loop = asyncio.get_event_loop()
    return await loop.run_in_executor(self.executor, partial(fn, *args, **kwargs))
```

---

## 2. TerminationReason 和 TerminationEvent

### 2.1 TerminationReason 枚举

```python
class TerminationReason(Enum):
    MAX_PROMPT_LENGTH_EXCEEDED = "max_prompt_length_exceeded"   # prompt 超长
    MAX_RESPONSE_LENGTH_EXCEEDED = "max_response_length_exceeded"  # 响应超长
    ENV_DONE = "env_done"           # 环境标记完成
    MAX_TURNS_EXCEEDED = "max_turns_exceeded"  # 超过最大轮数
    TIMEOUT = "timeout"             # 超时
    UNKNOWN = "unknown"             # 未知原因
    ERROR = "error"                 # 发生错误
```

### 2.2 TerminationEvent 异常

```python
class TerminationEvent(Exception):
    def __init__(self, reason: TerminationReason = TerminationReason.UNKNOWN):
        super().__init__(f"Terminated: {reason}")
        self.reason = reason
```

**用途**：在 `run()` 方法中抛出此异常来提前终止工作流。

```python
if done:
    raise TerminationEvent(TerminationReason.ENV_DONE)

if current_turn >= max_turns:
    raise TerminationEvent(TerminationReason.MAX_TURNS_EXCEEDED)
```

---

## 3. SimpleWorkflow - 简单工作流

`SimpleWorkflow` 是最简单的工作流实现，适用于单次 LLM 调用即可获得结果的场景。

### 3.1 类定义

```python
class SimpleWorkflow(Workflow):
    def __init__(
        self,
        rollout_engine: RolloutEngine,
        reward_function: RewardFunction,
        **kwargs,
    ):
        super().__init__(rollout_engine, **kwargs)
        self.agent = SimpleAgent()
        self.reward_function = reward_function
```

### 3.2 SimpleAgent

```python
class SimpleAgent(BaseAgent):
    def __init__(self, **kwargs):
        self._trajectory = Trajectory()
    
    def reset(self):
        self._trajectory = Trajectory()
    
    def update_from_model(*args, **kwargs):
        pass
    
    def update_from_env(*args, **kwargs):
        pass
    
    @property
    def trajectory(self) -> Trajectory:
        return self._trajectory
```

### 3.3 工作流程

```python
async def run(self, task: dict, uid: str, **kwargs) -> Episode:
    # 重置组件
    self.reset(task, uid)
    
    # 从任务中提取消息
    if task.get("messages") is not None:
        messages = task["messages"]
    elif task.get("question") is not None:
        messages = [{"role": "user", "content": task["question"]}]
    # ... 其他情况
    
    # 获取模型响应
    output: ModelOutput = await self.rollout_engine.get_model_response(
        messages, application_id=uid, **kwargs
    )
    
    # 创建动作和计算奖励
    action = Action(action=output.content)
    reward_result = self.reward_function(task, action)
    
    # 创建 Step 并添加到轨迹
    trajectory = self.agent.trajectory
    trajectory.steps.append(Step(
        chat_completions=messages + [{"role": "assistant", "content": output.content, "reasoning": output.reasoning}],
        thought=output.reasoning,
        action=action,
        reward=reward_result.reward,
        model_output=output,
    ))
    
    # 提交轨迹
    self.commit(agent=self.agent, reset=True)
    
    # 处理终止原因
    if output.finish_reason == "length":
        raise TerminationEvent(TerminationReason.MAX_RESPONSE_LENGTH_EXCEEDED)
    
    raise TerminationEvent(TerminationReason.ENV_DONE)
```

### 3.4 使用场景

- 数学问题求解（单次回答）
- 问答任务
- 分类任务
- 任何只需一次 LLM 调用即可完成的任务

---

## 4. MultiTurnWorkflow - 多轮工作流

`MultiTurnWorkflow` 支持 Agent 和环境之间的多轮交互。

### 4.1 类定义

```python
class MultiTurnWorkflow(TimingTrackingMixin, Workflow):
    def __init__(
        self,
        agent_cls,                    # Agent 类（或字符串名称）
        env_cls,                      # Environment 类（或字符串名称）
        agent_args=None,              # Agent 参数
        env_args=None,                # Environment 参数
        max_steps=5,                  # 最大步数
        **kwargs,
    ):
        super().__init__(**kwargs)
        
        # 解析类名
        agent_cls = AGENT_CLASS_MAPPING[agent_cls] if isinstance(agent_cls, str) else agent_cls
        env_cls = ENV_CLASS_MAPPING[env_cls] if isinstance(env_cls, str) else env_cls
        
        self.agent = agent_cls(**(agent_args or {}))
        self.env = env_cls(**(env_args or {}))
        self.max_steps = max_steps
```

### 4.2 工作流程

```python
async def run(self, task: dict, uid: str, **kwargs) -> Episode | None:
    # 重置环境
    observation, info = await self.timed_env_call(self.reset, task=task, uid=uid)
    
    # Agent 接收初始观察
    self.agent.update_from_env(observation, 0, False, info)
    
    # 多轮交互
    for _ in range(1, self.max_steps + 1):
        # 获取模型响应
        output: ModelOutput = await self.timed_llm_call(
            self.agent.chat_completions, application_id=uid, **kwargs
        )
        response = output.text
        
        # Agent 处理响应
        action = self.agent.update_from_model(response)
        
        # 环境执行动作
        next_obs, reward, done, info = await self.timed_env_call(
            self.env.step, action
        )
        
        # Agent 接收环境反馈
        self.agent.update_from_env(next_obs, reward, done, info)
        
        # 检查终止条件
        if output.finish_reason == "length":
            raise TerminationEvent(TerminationReason.MAX_RESPONSE_LENGTH_EXCEEDED)
        
        if done:
            raise TerminationEvent(TerminationReason.ENV_DONE)
    
    raise TerminationEvent(TerminationReason.MAX_TURNS_EXCEEDED)
```

### 4.3 TimingTrackingMixin

`MultiTurnWorkflow` 继承了 `TimingTrackingMixin`，提供了计时功能：

```python
class TimingTrackingMixin:
    async def timed_env_call(self, fn, *args, **kwargs):
        """计时环境调用"""
        start = time.time()
        result = fn(*args, **kwargs)
        if asyncio.iscoroutine(result):
            result = await result
        self.env_time += time.time() - start
        return result
    
    async def timed_llm_call(self, *args, **kwargs):
        """计时 LLM 调用"""
        start = time.time()
        result = await self.rollout_engine.get_model_response(*args, **kwargs)
        self.llm_time += time.time() - start
        return result
```

### 4.4 使用场景

- 需要多轮对话的任务
- 需要环境反馈的交互式任务
- 游戏类任务（如 FrozenLake）

---

## 5. SingleTurnWorkflow - 单轮工作流

`SingleTurnWorkflow` 是 `MultiTurnWorkflow` 的特例，`max_steps=1`。

```python
class SingleTurnWorkflow(MultiTurnWorkflow):
    def __init__(self, agent_cls, env_cls, agent_args=None, env_args=None, **kwargs):
        super().__init__(
            agent_cls=agent_cls,
            env_cls=env_cls,
            agent_args=agent_args,
            env_args=env_args,
            max_steps=1,
            **kwargs,
        )
```

---

## 6. CumulativeWorkflow - 累积工作流

`CumulativeWorkflow` 用于累积型对话完成，每一步的对话历史都是前一步的超集。

### 6.1 特点

- 适用于需要保留完整对话历史的场景
- 每一步都包含之前所有的对话内容
- 适合训练需要完整上下文的任务

---

## 7. Store - 跨 Episode 共享存储

`Store` 提供了一种在所有工作流实例之间共享状态的机制。

```python
class Store:
    """跨 Episode 共享存储"""
    
    def __init__(self):
        self._data = {}
    
    def get(self, key, default=None):
        return self._data.get(key, default)
    
    def set(self, key, value):
        self._data[key] = value
```

**用途**：
- 共享统计信息
- 维护全局状态
- 跨任务传递信息

---

## 8. 如何自定义 Workflow

### 8.1 最小实现

```python
from rllm.workflows import Workflow, TerminationEvent, TerminationReason
from rllm.agents import BaseAgent, Episode

class MyWorkflow(Workflow):
    def __init__(self, rollout_engine, **kwargs):
        super().__init__(rollout_engine, **kwargs)
        self.agent = MyAgent()
    
    async def run(self, task: dict, uid: str, **kwargs) -> Episode:
        # 重置
        self.reset(task, uid)
        
        # 准备消息
        messages = [{"role": "user", "content": task["question"]}]
        
        # 获取模型响应
        output = await self.rollout_engine.get_model_response(messages)
        
        # 创建轨迹
        self.agent.trajectory.steps.append(Step(
            chat_completions=messages + [{"role": "assistant", "content": output.content}],
            model_output=output,
        ))
        
        # 提交
        self.commit(agent=self.agent, reset=True)
        
        raise TerminationEvent(TerminationReason.ENV_DONE)
```

### 8.2 多 Agent 工作流

```python
class MultiAgentWorkflow(Workflow):
    def __init__(self, rollout_engine, **kwargs):
        super().__init__(rollout_engine, **kwargs)
        self.solver = SolverAgent()
        self.judge = JudgeAgent()
    
    async def run(self, task: dict, uid: str, **kwargs) -> Episode:
        self.reset(task, uid)
        
        # Solver 生成答案
        solver_messages = [{"role": "user", "content": task["question"]}]
        solver_output = await self.rollout_engine.get_model_response(solver_messages)
        answer = solver_output.content
        
        # Judge 评估答案
        judge_messages = [
            {"role": "user", "content": f"问题: {task['question']}\n答案: {answer}\n是否正确?"}
        ]
        judge_output = await self.rollout_engine.get_model_response(judge_messages)
        is_correct = "是" in judge_output.content
        
        # 创建轨迹
        self.solver.trajectory.steps.append(Step(
            chat_completions=solver_messages + [{"role": "assistant", "content": answer}],
            model_output=solver_output,
        ))
        
        self.judge.trajectory.steps.append(Step(
            chat_completions=judge_messages + [{"role": "assistant", "content": judge_output.content}],
            model_output=judge_output,
        ))
        
        # 提交两个轨迹
        self.commit(name="solver", agent=self.solver, reset=True)
        self.commit(name="judge", agent=self.judge, reset=True)
        
        raise TerminationEvent(TerminationReason.ENV_DONE)
```

---

## 9. Workflow 在训练中的角色

```
┌─────────────────────────────────────────────────────────┐
│                    Training Loop                         │
│                                                          │
│  ┌──────────────────────────────────────────────────┐   │
│  │              Workflow Engine                      │   │
│  │                                                    │   │
│  │  ┌─────────┐    ┌─────────┐    ┌───────────────┐ │   │
│  │  │         │    │         │    │               │ │   │
│  │  │ Workflow│───▶│ Rollout │───▶│   Episodes    │ │   │
│  │  │ .run()  │    │ Engine  │    │  Collected    │ │   │
│  │  │         │    │         │    │               │ │   │
│  │  └─────────┘    └─────────┘    └───────┬───────┘ │   │
│  │                                         │         │   │
│  └─────────────────────────────────────────┼─────────┘   │
│                                            │             │
│                                            ▼             │
│  ┌─────────────────────────────────────────────────┐    │
│  │           Transform Pipeline                     │    │
│  │  Episodes → TrajectoryGroups → Advantages       │    │
│  └─────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────┘
```

1. **执行工作流**：Workflow Engine 并行运行 N 个 Workflow 实例
2. **收集 Episodes**：每个 Workflow 实例返回一个 Episode
3. **转换数据**：Episodes 被转换为 TrajectoryGroups 用于优势计算
4. **训练更新**：使用 TrajectoryGroups 更新策略

---

## 10. 总结

| 类 | 用途 | 特点 |
|----|------|------|
| `Workflow` | 抽象基类 | 定义标准接口，提供奖励计算和后处理 |
| `SimpleWorkflow` | 简单工作流 | 单次 LLM 调用，适合问答任务 |
| `SingleTurnWorkflow` | 单轮工作流 | `max_steps=1`，Agent + Environment |
| `MultiTurnWorkflow` | 多轮工作流 | 支持多轮交互，带计时功能 |
| `CumulativeWorkflow` | 累积工作流 | 保留完整对话历史 |
| `DistillationWorkflow` | 蒸馏工作流 | 用于知识蒸馏训练 |
| `Store` | 共享存储 | 跨 Episode 共享状态 |

理解 Workflow 模块后，建议继续学习 Engine 模块，了解推理和执行的具体实现。
