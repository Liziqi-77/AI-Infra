# KernelGYM 项目技术分析报告

## 目录

1. [项目概述](#1-项目概述)
2. [整体架构设计](#2-整体架构设计)
3. [核心模块详解](#3-核心模块详解)
4. [关键技术栈与选型依据](#4-关键技术栈与选型依据)
5. [数据流程设计](#5-数据流程设计)
6. [接口定义规范](#6-接口定义规范)
7. [设计模式应用](#7-设计模式应用)
8. [性能优化策略](#8-性能优化策略)
9. [扩展性设计](#9-扩展性设计)
10. [DR.Kernel 训练框架](#10-drkernel-训练框架)
11. [部署与运维](#11-部署与运维)

---

## 1. 项目概述

### 1.1 项目背景

**KernelGYM** 是由 HKUST-NLP 团队开发的 GPU 分布式环境，专门用于评估和训练 AI 模型进行 GPU 内核生成任务。该项目是论文 **"Dr.Kernel: Reinforcement Learning Done Right for Triton Kernel Generations"** 的核心实现，为强化学习训练 GPU 内核生成模型提供了完整的解决方案。

### 1.2 核心价值

KernelGYM 解决了 GPU 内核生成评估中的独特挑战：

| 挑战 | 解决方案 |
|------|----------|
| **GPU 资源管理** | 子进程隔离架构，CUDA 错误不会影响主评估流程 |
| **性能测量复杂性** | 内置 CUDA 事件计时，支持可配置的预热和试验次数 |
| **正确性验证** | 支持自定义容差级别（rtol/atol）、多测试用例、诱饵内核检测 |
| **可扩展性** | Redis 任务队列支持从单 GPU 到多节点部署的无缝扩展 |

### 1.3 主要功能

- **长周期 RL 训练**：支持多轮 rollouts、奖励黑客检测、详细性能分析指标
- **智能体轨迹收集**：从智能体与内核评估环境的交互中收集高质量训练数据
- **大规模内核优化**：跨数千个任务并行部署智能体优化内核实现
- **并行内核评估**：在分布式 GPU 集群上评估内核正确性和性能，自动错误恢复

---

## 2. 整体架构设计

### 2.1 架构总览图

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                          KernelGYM Architecture                              │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────────────┐           │
│  │   Client     │───▶│  API Server  │───▶│   Task Manager       │           │
│  │  (Training)  │    │  (FastAPI)   │    │   (Redis Queue)      │           │
│  └──────────────┘    └──────────────┘    └──────────┬───────────┘           │
│                                                      │                       │
│                      ┌───────────────────────────────┴───────────┐           │
│                      │              Worker Layer                  │           │
│  ┌───────────────────┼───────────────────────────────────────────┤           │
│  │                   ▼                                           │           │
│  │  ┌─────────────────────┐  ┌─────────────────────┐             │           │
│  │  │   GPU Worker 0      │  │   GPU Worker N      │             │           │
│  │  │  ┌───────────────┐  │  │  ┌───────────────┐  │             │           │
│  │  │  │ Subprocess    │  │  │  │ Subprocess    │  │    ...      │           │
│  │  │  │ Pool (CUDA    │  │  │  │ Pool (CUDA    │  │             │           │
│  │  │  │ Isolation)    │  │  │  │ Isolation)    │  │             │           │
│  │  │  └───────────────┘  │  │  └───────────────┘  │             │           │
│  │  └─────────────────────┘  └─────────────────────┘             │           │
│  │                                                               │           │
│  └───────────────────────────────────────────────────────────────┘           │
│                                                                              │
│  ┌───────────────────────────────────────────────────────────────┐           │
│  │                     Core Components                            │           │
│  │  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────┐    │           │
│  │  │  Backends   │  │  Toolkits   │  │  Workflow Controllers│    │           │
│  │  │  (Compile/  │  │  (Evaluate) │  │  (Orchestrate)       │    │           │
│  │  │   Load/Run) │  │             │  │                      │    │           │
│  │  └─────────────┘  └─────────────┘  └─────────────────────┘    │           │
│  └───────────────────────────────────────────────────────────────┘           │
│                                                                              │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 2.2 分层架构

项目采用清晰的分层架构设计：

```
┌─────────────────────────────────────────────────────────────┐
│                    Presentation Layer                        │
│  ┌─────────────────────────────────────────────────────────┐ │
│  │  FastAPI Server (REST API Endpoints)                    │ │
│  │  - /evaluate, /evaluate/batch                           │ │
│  │  - /workflow/submit, /workflow/results/{task_id}        │ │
│  │  - /worker/register, /worker/heartbeat                  │ │
│  └─────────────────────────────────────────────────────────┘ │
├─────────────────────────────────────────────────────────────┤
│                    Orchestration Layer                       │
│  ┌─────────────────────────────────────────────────────────┐ │
│  │  Workflow Controllers                                    │ │
│  │  - KernelBenchWorkflowController                         │ │
│  │  - KernelSimpleWorkflowController                        │ │
│  └─────────────────────────────────────────────────────────┘ │
├─────────────────────────────────────────────────────────────┤
│                    Task Management Layer                     │
│  ┌─────────────────────────────────────────────────────────┐ │
│  │  TaskManager + Scheduler                                 │ │
│  │  - Redis-based Task Queue                                │ │
│  │  - Priority Scheduling                                   │ │
│  │  - Worker Load Balancing                                 │ │
│  └─────────────────────────────────────────────────────────┘ │
├─────────────────────────────────────────────────────────────┤
│                    Execution Layer                           │
│  ┌─────────────────────────────────────────────────────────┐ │
│  │  GPU Workers + Subprocess Pool                           │ │
│  │  - CUDA Error Isolation                                  │ │
│  │  - Auto-restart on Failure                               │ │
│  └─────────────────────────────────────────────────────────┘ │
├─────────────────────────────────────────────────────────────┤
│                    Core Abstraction Layer                    │
│  ┌──────────────────┬──────────────────┬───────────────────┐ │
│  │     Backend      │     Toolkit      │     Schema        │ │
│  │  (Compile/Load/  │   (Evaluate)     │   (Data Models)   │ │
│  │      Run)        │                  │                   │ │
│  └──────────────────┴──────────────────┴───────────────────┘ │
└─────────────────────────────────────────────────────────────┘
```

### 2.3 目录结构

```
KernelGYM/
├── kernelgym/                    # 核心评估环境
│   ├── backend/                  # 后端抽象层
│   │   ├── base.py              # Backend 基类
│   │   ├── registry.py          # 后端注册表
│   │   └── kernelbench/         # KernelBench 后端实现
│   │       ├── base.py          # 基础后端
│   │       ├── cuda_backend.py  # CUDA 后端
│   │       └── triton_backend.py # Triton 后端
│   ├── toolkit/                  # 工具包抽象层
│   │   ├── base.py              # Toolkit 基类
│   │   ├── registry.py          # 工具包注册表
│   │   ├── kernelbench/         # KernelBench 评估工具
│   │   │   ├── toolkit.py       # 主工具类
│   │   │   ├── pipeline.py      # 评估流水线
│   │   │   ├── correctness.py   # 正确性检查
│   │   │   ├── profiling.py     # 性能分析
│   │   │   └── timing.py        # 时间测量
│   │   └── kernel_simple/       # 简化评估工具
│   ├── core/                     # 核心抽象
│   │   ├── types.py             # 数据类型定义
│   │   ├── scheduler.py         # 调度器接口
│   │   ├── workflow.py          # 工作流控制器
│   │   └── registry.py          # 注册表
│   ├── schema/                   # 数据模型
│   │   ├── task.py              # 任务定义
│   │   ├── result.py            # 结果定义
│   │   └── simple_task.py       # 简化任务
│   ├── server/                   # 服务层
│   │   ├── api/                 # API 服务
│   │   │   ├── server.py        # FastAPI 服务
│   │   │   ├── models.py        # 请求/响应模型
│   │   │   └── monitoring_routes.py # 监控路由
│   │   ├── task_manager.py      # 任务管理器
│   │   ├── scheduler.py         # 调度器适配器
│   │   └── code_retry_manager.py # 重试管理
│   ├── worker/                   # 工作进程
│   │   ├── gpu_worker.py        # GPU Worker
│   │   ├── task_executor.py     # 任务执行器
│   │   ├── subprocess_pool.py   # 子进程池
│   │   └── worker_monitor.py    # Worker 监控
│   ├── workflow/                 # 工作流实现
│   │   ├── kernelbench.py       # KernelBench 工作流
│   │   ├── kernelbench_types.py # 类型定义
│   │   └── kernelbench_helpers.py # 辅助函数
│   ├── config/                   # 配置管理
│   │   └── settings.py          # 配置类
│   └── utils/                    # 工具函数
│       ├── error_classifier.py  # 错误分类
│       └── gpu_diagnostics.py   # GPU 诊断
├── drkernel/                     # DR.Kernel 训练框架
│   ├── kernel/                   # 内核训练模块
│   │   ├── main_kernel.py       # 主入口
│   │   ├── kernel_trainer.py    # 训练器
│   │   ├── rewards/             # 奖励函数
│   │   │   ├── kernel_reward.py # 内核奖励
│   │   │   └── reward_client.py # 奖励客户端
│   │   ├── trainer/             # 训练算法
│   │   │   └── ppo/             # PPO 算法
│   │   ├── workers/             # 工作进程
│   │   │   ├── agent/           # 智能体
│   │   │   └── reward_manager/  # 奖励管理
│   │   └── scripts/             # 训练脚本
│   │       ├── rl/              # RL 训练脚本
│   │       ├── eval/            # 评估脚本
│   │       └── sft/             # SFT 训练脚本
│   └── verl_patch/               # VERL 框架补丁
│       ├── trainer/code/        # 训练器补丁
│       │   ├── ppo/             # PPO 算法增强
│       │   ├── metrics/         # 指标跟踪
│       │   └── filters/         # 过滤器
│       ├── workers/code/        # Worker 补丁
│       │   ├── agent/           # 智能体实现
│       │   ├── reward_manager/  # 奖励管理器
│       │   └── rollout/         # Rollout 实现
│       └── utils/               # 工具函数
├── scripts/                      # 部署脚本
│   ├── auto_configure.sh        # 自动配置
│   ├── start_all_with_monitor.sh # 启动脚本
│   └── start_worker_multinode.sh # 多节点启动
└── requirements.txt              # 依赖列表
```

---

## 3. 核心模块详解

### 3.1 Backend 模块

Backend 模块负责内核代码的编译、加载和执行，是整个系统的底层执行引擎。

#### 3.1.1 抽象基类设计

```python
# kernelgym/backend/base.py
class Backend(ABC):
    name: str = "unknown"

    @abstractmethod
    def compile(self, code: str, **kwargs: Any) -> Dict[str, Any]:
        """编译内核代码并返回构建元数据"""

    @abstractmethod
    def load(self, artifact: Dict[str, Any], **kwargs: Any) -> Any:
        """加载编译产物以供执行"""

    @abstractmethod
    def run(self, handle: Any, inputs: Dict[str, Any], **kwargs: Any) -> Dict[str, Any]:
        """执行并返回运行时指标"""

    def create_model(self, handle: Any, init_inputs: Any, **kwargs: Any) -> Any:
        """可选钩子：从加载的句柄构造模型实例"""

    def open_session(self, handle: Any, device: Any | None = None) -> "BackendSession":
        """创建会话用于生命周期管理"""

    def cleanup(self, handle: Any, **kwargs: Any) -> None:
        """可选清理钩子"""
```

#### 3.1.2 Backend 会话模式

```python
class BackendSession:
    """轻量级生命周期包装器"""

    def __init__(self, backend: Backend, handle: Any, device: Any | None = None):
        self.backend = backend
        self.handle = handle
        self.device = device

    def __enter__(self) -> "BackendSession":
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.cleanup()
```

#### 3.1.3 支持的后端类型

| 后端 | 描述 | 用例 |
|------|------|------|
| **kernelbench.cuda** | CUDA 内核后端 | 使用 PyTorch CUDA 扩展或内联 CUDA 代码 |
| **kernelbench.triton** | Triton 内核后端 | 使用 OpenAI Triton 语言编写内核 |

### 3.2 Toolkit 模块

Toolkit 模块实现了评估逻辑，负责正确性检查和性能测量。

#### 3.2.1 抽象基类

```python
# kernelgym/toolkit/base.py
class Toolkit(ABC):
    name: str = "unknown"

    @abstractmethod
    def evaluate(self, task: Dict[str, Any], backend: Backend, **kwargs: Any) -> Dict[str, Any]:
        """针对后端运行评估逻辑"""
```

#### 3.2.2 KernelBench Toolkit 实现

```python
class KernelBenchToolkit(Toolkit):
    name = "kernelbench"

    def evaluate(self, task: Dict[str, Any], backend=None, **kwargs) -> Dict[str, Any]:
        task_type = task.get("task_type", "evaluation")
        
        if task_type == "evaluation":
            return self.evaluate_kernel(EvaluationTask.from_dict(task), ...)
        elif task_type == "reference_timing":
            return self.evaluate_reference_timing(ReferenceTimingTask.from_dict(task), ...)
        elif task_type == "kernel_evaluation":
            return self.evaluate_kernel_only(KernelEvaluationTask.from_dict(task), ...)
```

#### 3.2.3 评估流程

```
┌─────────────────────────────────────────────────────────────────┐
│                    KernelBench Evaluation Flow                   │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────────┐   │
│  │ Code         │───▶│ Compilation  │───▶│ Correctness      │   │
│  │ Validation   │    │ & Loading     │    │ Checking         │   │
│  └──────────────┘    └──────────────┘    └────────┬─────────┘   │
│                                                    │             │
│                                                    ▼             │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────────┐   │
│  │ Decoy        │◀───│ Performance  │◀───│ Triton           │   │
│  │ Detection    │    │ Profiling    │    │ Detection        │   │
│  └──────────────┘    └──────────────┘    └──────────────────┘   │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### 3.3 Workflow Controller 模块

Workflow Controller 负责编排多步骤评估工作流。

#### 3.3.1 基类设计

```python
# kernelgym/core/workflow.py
class WorkflowController(ABC):
    @abstractmethod
    async def handle_request(self, input_data: Dict[str, Any], scheduler: SchedulerAPI) -> Dict[str, Any]:
        """运行工作流并返回最终响应"""

    async def validate_request(self, input_data: Dict[str, Any]) -> Dict[str, Any]:
        """可选请求验证钩子"""

    async def on_task_finished(self, state: WorkflowState, task_id: str, result: Dict[str, Any], scheduler: SchedulerAPI) -> Optional[Dict[str, Any]]:
        """可选增量决策钩子"""

    async def aggregate(self, state: WorkflowState) -> Dict[str, Any]:
        """聚合状态为最终响应"""
```

#### 3.3.2 KernelBench 工作流

```python
class KernelBenchWorkflowController(WorkflowController):
    async def handle_request(self, input_data: Dict[str, Any], scheduler: SchedulerAPI) -> Dict[str, Any]:
        # 1. 验证输入
        validation = self._validate_inputs(eval_task)
        
        # 2. 创建内核评估任务
        kernel_task_spec = TaskSpec(kind="kernelbench.kernel", payload=kernel_payload, ...)
        kernel_task_id = await scheduler.submit(kernel_task_spec)
        kernel_result = await scheduler.wait(kernel_task_id)
        
        # 3. 如果编译和正确性通过，执行参考计时
        if kernel_result.compiled and kernel_result.correctness:
            ref_task_spec = TaskSpec(kind="kernelbench.ref", payload=ref_payload, ...)
            ref_task_id = await scheduler.submit(ref_task_spec)
            ref_result = await scheduler.wait(ref_task_id)
        
        # 4. 合并结果并计算加速比
        combined = _combine_results(ref_result, kernel_result)
        return combined.to_dict()
```

### 3.4 Task Manager 模块

Task Manager 是任务调度和管理的核心组件。

#### 3.4.1 核心职责

```python
class TaskManager:
    def __init__(self, redis_client: redis.Redis):
        self.redis = redis_client
        self.priority_queues = {
            Priority.HIGH: f"{prefix}:queue:priority:high",
            Priority.NORMAL: f"{prefix}:queue:priority:normal",
            Priority.LOW: f"{prefix}:queue:priority:low",
        }
        self.worker_load_balancer = WorkerLoadBalancer()
        self.retry_manager = CodeRetryManager(redis_client)
```

#### 3.4.2 任务生命周期

```
┌─────────────────────────────────────────────────────────────────┐
│                       Task Lifecycle                             │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  ┌──────────┐    ┌──────────┐    ┌─────────────┐    ┌────────┐ │
│  │ PENDING  │───▶│PROCESSING│───▶│ COMPLETED/  │───▶│ RESULT │ │
│  │          │    │          │    │ FAILED      │    │ STORED │ │
│  └──────────┘    └──────────┘    └─────────────┘    └────────┘ │
│       │              │                   │                       │
│       │              │                   │                       │
│       ▼              ▼                   ▼                       │
│  ┌──────────┐    ┌──────────┐    ┌─────────────┐               │
│  │ Priority │    │ Worker   │    │ Error       │               │
│  │ Queue    │    │ Assigned │    │ Handling    │               │
│  └──────────┘    └──────────┘    └─────────────┘               │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### 3.5 GPU Worker 模块

GPU Worker 是实际执行评估任务的工作进程。

#### 3.5.1 Worker 架构

```python
class GPUWorker:
    def __init__(self, worker_id: str, device: str, redis_client: redis.Redis):
        self.worker_id = worker_id
        self.device = device
        self.worker_pool: Optional[SubprocessWorkerPool] = None
        self.pool_size = settings.worker_pool_size  # 默认 1
        self.max_tasks_per_worker = settings.max_tasks_per_worker  # 默认 1

    async def start(self):
        # 1. 初始化 GPU（使用 nvidia-smi，不初始化 CUDA）
        await self._initialize_gpu()
        
        # 2. 初始化子进程池
        self.worker_pool = SubprocessWorkerPool(
            device_id=self.device_id,
            pool_size=self.pool_size,
            max_tasks_per_worker=self.max_tasks_per_worker
        )
        
        # 3. 启动心跳和处理循环
        await asyncio.gather(
            self._heartbeat_loop(),
            self._processing_loop()
        )
```

#### 3.5.2 子进程隔离架构

```
┌─────────────────────────────────────────────────────────────────┐
│                    Subprocess Isolation                          │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │                    Main Process                            │  │
│  │  ┌─────────────────────────────────────────────────────┐  │  │
│  │  │ GPU Worker (No CUDA)                                │  │  │
│  │  │ - Task fetching from Redis                          │  │  │
│  │  │ - Worker pool management                            │  │  │
│  │  │ - Heartbeat & monitoring                            │  │  │
│  │  └─────────────────────────────────────────────────────┘  │  │
│  └───────────────────────────────────────────────────────────┘  │
│                              │                                   │
│                              │ Task Queue                        │
│                              ▼                                   │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │                 Subprocess Pool                            │  │
│  │  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐        │  │
│  │  │ Worker 0    │  │ Worker 1    │  │ Worker N    │        │  │
│  │  │ (CUDA Init) │  │ (CUDA Init) │  │ (CUDA Init) │        │  │
│  │  │             │  │             │  │             │        │  │
│  │  │ Isolated    │  │ Isolated    │  │ Isolated    │        │  │
│  │  │ CUDA Error  │  │ CUDA Error  │  │ CUDA Error  │        │  │
│  │  │ Recovery    │  │ Recovery    │  │ Recovery    │        │  │
│  │  └─────────────┘  └─────────────┘  └─────────────┘        │  │
│  └───────────────────────────────────────────────────────────┘  │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### 3.6 Subprocess Worker Pool

子进程工作池实现了 CUDA 错误的完全隔离和自动恢复。

#### 3.6.1 核心特性

```python
class SubprocessWorkerPool:
    """
    核心特性：
    1. 预先启动一组 worker 进程，复用处理多个任务
    2. torch 和 CUDA 只在启动时初始化一次
    3. 第一次遇到 CUDA error 时立即关闭 worker 进程
    4. 主进程自动重启新的 worker 进程
    5. 大幅降低 spawn 开销
    """

    def __init__(self, device_id: int, pool_size: int = 2, max_tasks_per_worker: int = 100):
        self.device_id = device_id
        self.pool_size = pool_size
        self.workers: List[PersistentWorker] = []

    async def execute_task(self, task_data: Dict, timeout: int = 60, max_retries: int = 2) -> Dict:
        worker = await self._get_idle_worker(timeout=timeout)
        result = await loop.run_in_executor(None, worker.execute_task, task_data, timeout)
        
        # 检查 worker 是否需要重启
        if not worker.is_alive():
            await self._restart_worker(worker)
        
        return result
```

#### 3.6.2 CUDA 错误处理流程

```
┌─────────────────────────────────────────────────────────────────┐
│                   CUDA Error Recovery Flow                       │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  ┌──────────────┐                                               │
│  │ Task         │                                               │
│  │ Execution    │                                               │
│  └──────┬───────┘                                               │
│         │                                                        │
│         ▼                                                        │
│  ┌──────────────┐    Yes    ┌──────────────────────────────┐   │
│  │ CUDA Error?  │──────────▶│ 1. Return error with         │   │
│  └──────┬───────┘           │    worker_exiting=True       │   │
│         │ No                │ 2. Aggressive GPU cleanup     │   │
│         ▼                   │ 3. Worker process exits       │   │
│  ┌──────────────┐           └──────────────────────────────┘   │
│  │ Return       │                      │                        │
│  │ Result       │                      ▼                        │
│  └──────────────┘           ┌──────────────────────────────┐   │
│                              │ Main Process:                │   │
│                              │ 1. Detect worker_exiting     │   │
│                              │ 2. Mark worker as dead       │   │
│                              │ 3. Restart new worker        │   │
│                              │ 4. Add to idle pool          │   │
│                              └──────────────────────────────┘   │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

---

## 4. 关键技术栈与选型依据

### 4.1 技术栈概览

| 层级 | 技术选型 | 选型依据 |
|------|----------|----------|
| **Web 框架** | FastAPI | 高性能异步支持、自动 API 文档、类型提示 |
| **任务队列** | Redis | 高性能、支持优先级队列、分布式友好 |
| **GPU 计算** | PyTorch + CUDA | 成熟生态、与 Triton 集成良好 |
| **内核语言** | Triton / CUDA | Triton 更易编写，CUDA 性能最优 |
| **RL 框架** | VERL | 分布式 RL 训练、Ray 集成 |
| **配置管理** | Pydantic Settings | 类型安全、环境变量支持 |
| **进程隔离** | multiprocessing.spawn | 完全隔离 CUDA 状态 |

### 4.2 依赖清单

```
# 核心依赖
torch>=2.0.0                    # GPU 计算框架
uvicorn[standard]==0.24.0       # ASGI 服务器
pydantic-settings==2.10.1       # 配置管理
python-multipart==0.0.6         # 文件上传支持

# 任务队列
redis==5.0.1                    # Redis 客户端
celery==5.3.4                   # 分布式任务队列
kombu==5.3.4                    # 消息传递抽象

# 数据库和缓存
sqlalchemy==2.0.23              # ORM
alembic==1.12.1                 # 数据库迁移

# 监控和日志
structlog==15.1.0               # 结构化日志

# GPU 和 ML
nvidia-ml-py3==7.352.0          # NVIDIA 管理库

# 工具
tenacity==8.2.3                 # 重试机制
psutil==5.9.6                   # 系统监控
aiohttp==3.9.1                  # 异步 HTTP 客户端
```

### 4.3 Triton vs CUDA 选型

| 特性 | Triton | CUDA |
|------|--------|------|
| **开发效率** | 高（Python-like 语法） | 低（需要 C++/CUDA） |
| **性能上限** | 接近 CUDA | 最优 |
| **学习曲线** | 平缓 | 陡峭 |
| **调试难度** | 较低 | 较高 |
| **适用场景** | 快速原型、一般优化 | 极致性能优化 |

---

## 5. 数据流程设计

### 5.1 评估请求流程

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                        Evaluation Request Flow                               │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│  Client                                                                      │
│    │                                                                         │
│    │ POST /evaluate                                                          │
│    ▼                                                                         │
│  ┌──────────────────────────────────────────────────────────────────────┐   │
│  │ FastAPI Server                                                        │   │
│  │  ┌────────────────────────────────────────────────────────────────┐  │   │
│  │  │ 1. Validate request (EvaluationRequest)                        │  │   │
│  │  │ 2. Create WorkflowController                                   │  │   │
│  │  │ 3. Execute workflow                                            │  │   │
│  │  └────────────────────────────────────────────────────────────────┘  │   │
│  └──────────────────────────────────────────────────────────────────────┘   │
│    │                                                                         │
│    │ submit task                                                             │
│    ▼                                                                         │
│  ┌──────────────────────────────────────────────────────────────────────┐   │
│  │ TaskManager (Redis Queue)                                             │   │
│  │  ┌────────────────────────────────────────────────────────────────┐  │   │
│  │  │ 1. Store task in Redis (status: PENDING)                       │  │   │
│  │  │ 2. Push to priority queue                                      │  │   │
│  │  │ 3. Wait for worker to pick up                                  │  │   │
│  │  └────────────────────────────────────────────────────────────────┘  │   │
│  └──────────────────────────────────────────────────────────────────────┘   │
│    │                                                                         │
│    │ get_next_task()                                                         │
│    ▼                                                                         │
│  ┌──────────────────────────────────────────────────────────────────────┐   │
│  │ GPU Worker                                                            │   │
│  │  ┌────────────────────────────────────────────────────────────────┐  │   │
│  │  │ 1. Pop task from queue                                         │  │   │
│  │  │ 2. Update status to PROCESSING                                 │  │   │
│  │  │ 3. Execute via Subprocess Pool                                 │  │   │
│  │  │    a. Compile kernel code                                      │  │   │
│  │  │    b. Load and run                                             │  │   │
│  │  │    c. Check correctness                                        │  │   │
│  │  │    d. Profile performance                                      │  │   │
│  │  │ 4. Return result                                               │  │   │
│  │  └────────────────────────────────────────────────────────────────┘  │   │
│  └──────────────────────────────────────────────────────────────────────┘   │
│    │                                                                         │
│    │ complete_task()                                                         │
│    ▼                                                                         │
│  ┌──────────────────────────────────────────────────────────────────────┐   │
│  │ Result Storage                                                        │   │
│  │  ┌────────────────────────────────────────────────────────────────┐  │   │
│  │  │ {                                                               │  │   │
│  │  │   "task_id": "xxx",                                            │  │   │
│  │  │   "compiled": true,                                            │  │   │
│  │  │   "correctness": true,                                         │  │   │
│  │  │   "kernel_runtime": 0.123,                                     │  │   │
│  │  │   "reference_runtime": 0.456,                                  │  │   │
│  │  │   "speedup": 3.71,                                             │  │   │
│  │  │   "profiling": {...}                                           │  │   │
│  │  │ }                                                               │  │   │
│  │  └────────────────────────────────────────────────────────────────┘  │   │
│  └──────────────────────────────────────────────────────────────────────┘   │
│                                                                              │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 5.2 多轮 RL 训练数据流

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                     Multi-turn RL Training Data Flow                         │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│  ┌──────────────────────────────────────────────────────────────────────┐   │
│  │ Training Loop (Ray)                                                   │   │
│  │  ┌────────────────────────────────────────────────────────────────┐  │   │
│  │  │ for epoch in range(num_epochs):                                │  │   │
│  │  │     for batch in dataloader:                                   │  │   │
│  │  │         # 1. Generate rollouts                                 │  │   │
│  │  │         trajectories = actor.generate(batch)                   │  │   │
│  │  │                                                                │  │   │
│  │  │         # 2. Compute rewards (multi-turn)                      │  │   │
│  │  │         rewards = reward_fn(trajectories)                      │  │   │
│  │  │                                                                │  │   │
│  │  │         # 3. Update policy                                     │  │   │
│  │  │         loss = ppo_update(trajectories, rewards)               │  │   │
│  │  └────────────────────────────────────────────────────────────────┘  │   │
│  └──────────────────────────────────────────────────────────────────────┘   │
│                                                                              │
│  Reward Computation (per turn):                                              │
│  ┌──────────────────────────────────────────────────────────────────────┐   │
│  │ Turn 1: Generate initial kernel                                      │   │
│  │   └──▶ KernelGYM: Evaluate kernel → Reward r1                        │   │
│  │                                                                       │   │
│  │ Turn 2: Refine based on feedback                                     │   │
│  │   └──▶ KernelGYM: Evaluate refined kernel → Reward r2                │   │
│  │                                                                       │   │
│  │ Turn 3: Final optimization                                           │   │
│  │   └──▶ KernelGYM: Evaluate optimized kernel → Reward r3              │   │
│  │                                                                       │   │
│  │ Cumulative Reward: R = r1 + γ*r2 + γ²*r3                             │   │
│  └──────────────────────────────────────────────────────────────────────┘   │
│                                                                              │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 6. 接口定义规范

### 6.1 REST API 端点

#### 6.1.1 评估端点

```python
# POST /evaluate
# 提交内核评估任务
{
    "task_id": "softmax-kernel-001",
    "reference_code": "...",
    "kernel_code": "...",
    "entry_point": "Model",
    "backend": "triton",
    "num_correct_trials": 5,
    "num_perf_trials": 100,
    "timeout": 300,
    "priority": "normal"
}

# Response
{
    "task_id": "softmax-kernel-001",
    "status": "completed",
    "compiled": true,
    "correctness": true,
    "decoy_kernel": false,
    "reference_runtime": 0.456,
    "kernel_runtime": 0.123,
    "speedup": 3.71,
    "metadata": {
        "device": "cuda:0",
        "gpu_name": "NVIDIA H100",
        "profiling": {...}
    }
}
```

#### 6.1.2 工作流端点

```python
# POST /workflow/submit
# 提交通用工作流任务
{
    "workflow": "kernelbench",
    "task_id": "my-task-001",
    "payload": {
        "reference_code": "...",
        "kernel_code": "...",
        ...
    },
    "resources": {
        "gpus": 1
    }
}

# Response
{
    "task_id": "my-task-001",
    "status": "completed",
    "result": {...}
}
```

#### 6.1.3 Worker 管理端点

```python
# POST /worker/register
# 注册 Worker
{
    "worker_id": "node-1_gpu_0",
    "device": "cuda:0",
    "node_id": "node-1",
    "hostname": "worker-01"
}

# POST /worker/heartbeat
# Worker 心跳
{
    "worker_id": "node-1_gpu_0",
    "device": "cuda:0",
    "node_id": "node-1"
}

# GET /workers/status
# 获取所有 Worker 状态
{
    "node-1_gpu_0": {
        "device": "cuda:0",
        "status": "online",
        "last_heartbeat": "2025-01-15T10:30:00",
        "tasks_processed": 150
    }
}
```

### 6.2 数据模型

#### 6.2.1 任务模型

```python
@dataclass
class TaskSpec:
    kind: str                          # 任务类型
    payload: Dict[str, Any]            # 任务载荷
    resources: Optional[Dict] = None   # 资源需求
    metadata: Dict[str, Any] = field(default_factory=dict)

@dataclass
class EvaluationTask:
    task_id: str
    reference_code: str
    kernel_code: str
    entry_point: str = "Model"
    backend: str = "triton"
    device: str = "cuda:0"
    num_correct_trials: int = 5
    num_perf_trials: int = 100
    timeout: int = 300
    priority: Priority = Priority.NORMAL
    run_correctness: bool = True
    run_performance: bool = True
    enable_profiling: bool = False
```

#### 6.2.2 结果模型

```python
@dataclass
class EvaluationResult:
    task_id: str
    compiled: bool
    correctness: bool
    decoy_kernel: bool
    reference_runtime: float
    kernel_runtime: float
    speedup: float
    metadata: Dict[str, Any]
    status: str
    error_message: Optional[str] = None
    error_code: Optional[ErrorCode] = None

@dataclass
class KernelEvaluationResult:
    task_id: str
    base_task_id: str
    compiled: bool
    correctness: bool
    decoy_kernel: bool
    kernel_runtime: float
    metadata: Dict[str, Any]
    status: str
```

---

## 7. 设计模式应用

### 7.1 策略模式 (Strategy Pattern)

Backend 和 Toolkit 使用策略模式实现可扩展的评估策略：

```python
# Backend 策略
class Backend(ABC):
    @abstractmethod
    def compile(self, code: str, **kwargs) -> Dict: ...
    @abstractmethod
    def load(self, artifact: Dict, **kwargs) -> Any: ...
    @abstractmethod
    def run(self, handle: Any, inputs: Dict, **kwargs) -> Dict: ...

# 具体策略
class CUDABackend(Backend): ...
class TritonBackend(Backend): ...

# 策略选择
def get_backend(name: str) -> Backend:
    return _BACKEND_REGISTRY.get(name.lower())()
```

### 7.2 注册表模式 (Registry Pattern)

使用注册表模式管理可插拔组件：

```python
class Registry:
    def __init__(self):
        self._items: Dict[str, Any] = {}

    def register(self, name: str, obj: Any) -> None:
        if name in self._items:
            raise KeyError(f"Registry already contains '{name}'")
        self._items[name] = obj

    def get(self, name: str) -> Any:
        if name not in self._items:
            raise KeyError(f"Registry missing '{name}'")
        return self._items[name]

# 使用示例
_BACKEND_REGISTRY = Registry()
_BACKEND_REGISTRY.register("kernelbench", KernelBenchBackend)

def get_backend(name: str) -> Backend:
    return _BACKEND_REGISTRY.get(name.lower())()
```

### 7.3 模板方法模式 (Template Method Pattern)

WorkflowController 使用模板方法定义评估流程骨架：

```python
class WorkflowController(ABC):
    @abstractmethod
    async def handle_request(self, input_data: Dict, scheduler: SchedulerAPI) -> Dict:
        """子类实现具体流程"""

    async def validate_request(self, input_data: Dict) -> Dict:
        """可选钩子，默认实现"""
        return {"valid": True}

    async def on_task_finished(self, state, task_id, result, scheduler) -> Optional[Dict]:
        """可选钩子，用于增量决策"""
        return None

    async def aggregate(self, state: WorkflowState) -> Dict:
        """聚合状态为最终响应"""
        return dict(state.data)
```

### 7.4 工厂模式 (Factory Pattern)

使用工厂函数创建复杂对象：

```python
def get_toolkit(name: str) -> Toolkit:
    """工厂函数：创建 Toolkit 实例"""
    _ensure_default_toolkits()
    return _TOOLKIT_REGISTRY.get(name.lower())()

def get_backend(name: str) -> Backend:
    """工厂函数：创建 Backend 实例"""
    return _BACKEND_REGISTRY.get(name.lower())()
```

### 7.5 上下文管理器模式 (Context Manager Pattern)

BackendSession 使用上下文管理器管理资源生命周期：

```python
class BackendSession:
    def __enter__(self) -> "BackendSession":
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.cleanup()

# 使用示例
with backend.open_session(handle, device="cuda:0") as session:
    model = session.create_model(init_inputs)
    output = session.run(inputs)
```

### 7.6 观察者模式 (Observer Pattern)

Worker 心跳机制实现了观察者模式：

```python
class GPUWorker:
    async def _heartbeat_loop(self):
        """定期发送心跳通知服务器"""
        while self.running:
            ok = await self._send_heartbeat_to_api()
            if not ok:
                self.running = False
                break
            await self._update_worker_status(online=True)
            await asyncio.sleep(10)

# 服务器端观察者
class TaskManager:
    async def update_worker_heartbeat(self, worker_id: str):
        """接收并处理心跳"""
        now = datetime.now().isoformat()
        await self.redis.hset(
            f"{self.worker_prefix}{worker_id}",
            mapping={"last_heartbeat": now, "status": "online"}
        )
```

---

## 8. 性能优化策略

### 8.1 子进程池复用

**问题**：每次任务都 spawn 新进程，开销约 2.5 秒

**解决方案**：预先启动持久化 worker 进程池

```python
class SubprocessWorkerPool:
    def __init__(self, device_id: int, pool_size: int = 2, max_tasks_per_worker: int = 100):
        # 预先启动 worker 进程
        for i in range(pool_size):
            worker = PersistentWorker(worker_id, device_id, ...)
            self.workers.append(worker)
            self.idle_workers.append(worker)

    async def execute_task(self, task_data: Dict, timeout: int = 60) -> Dict:
        # 复用空闲 worker
        worker = await self._get_idle_worker()
        result = worker.execute_task(task_data, timeout)
        return result
```

**效果**：spawn 开销从 ~2.5s 降至几乎为 0

### 8.2 CUDA 错误隔离与自动恢复

**问题**：CUDA 错误会污染进程状态，导致后续任务失败

**解决方案**：子进程隔离 + 自动重启

```python
def _persistent_worker_loop(worker_id, device_id, task_queue, result_queue):
    while True:
        task_data = task_queue.get()
        try:
            result = _execute_task_in_worker(task_data, device, ...)
            result_queue.put(result)
        except Exception as e:
            if is_cuda_error(e):
                # CUDA 错误：标记退出，主进程会重启
                result_queue.put({
                    "success": False,
                    "worker_exiting": True,
                    ...
                })
                break  # 退出循环，进程终止
            else:
                # 非 CUDA 错误：继续运行
                result_queue.put({"success": False, ...})
```

### 8.3 GPU 内存管理

```python
def _aggressive_gpu_cleanup(device_id: int):
    """强力清理 GPU 显存"""
    import torch, gc

    # 1. 同步 CUDA 操作
    torch.cuda.synchronize(device_id)

    # 2. 清空 PyTorch 缓存
    torch.cuda.empty_cache()

    # 3. Python 垃圾回收
    gc.collect()

    # 4. 重置内存统计
    torch.cuda.reset_peak_memory_stats(device_id)
    torch.cuda.reset_accumulated_memory_stats(device_id)

    # 5. 最终同步
    torch.cuda.synchronize(device_id)
```

### 8.4 Redis 任务队列优化

```python
class TaskManager:
    def __init__(self, redis_client: redis.Redis):
        # 优先级队列
        self.priority_queues = {
            Priority.HIGH: f"{prefix}:queue:priority:high",
            Priority.NORMAL: f"{prefix}:queue:priority:normal",
            Priority.LOW: f"{prefix}:queue:priority:low",
        }

        # Worker 专属队列（减少竞争）
        self.worker_queues: Dict[str, str] = {}

    async def get_next_task(self, worker_id: str) -> Optional[Dict]:
        # 优先从 worker 专属队列获取
        worker_queue_key = f"{prefix}:queue:worker:{worker_id}"
        task_id = await self.redis.rpop(worker_queue_key)

        if task_id is None:
            # 回退到优先级队列
            for priority in (Priority.HIGH, Priority.NORMAL, Priority.LOW):
                queue_key = f"{prefix}:queue:priority:{priority.value}"
                task_id = await self.redis.rpop(queue_key)
                if task_id:
                    break
```

### 8.5 批量评估优化

```python
# 批量提交评估任务
@app.post("/evaluate/batch")
async def evaluate_batch(request: BatchEvaluationRequest, ...):
    batch_results = []
    for task_request in request.tasks:
        _, result, status = await _execute_workflow(
            task_mgr=task_mgr,
            workflow_name=task_request.workflow,
            payload=task_request.dict(),
        )
        batch_results.append(EvaluationResponse(status=status, **result))
    
    return BatchEvaluationResponse(
        batch_id=request.batch_id,
        total_tasks=len(request.tasks),
        results=batch_results,
    )
```

---

## 9. 扩展性设计

### 9.1 添加新 Backend

```python
# 1. 实现 Backend 基类
class MyBackend(Backend):
    name = "my_backend"

    def compile(self, code: str, **kwargs) -> Dict:
        # 编译逻辑
        return {"compiled": True, "artifact": ...}

    def load(self, artifact: Dict, **kwargs) -> Any:
        # 加载逻辑
        return handle

    def run(self, handle: Any, inputs: Dict, **kwargs) -> Dict:
        # 执行逻辑
        return {"output": result, "runtime": elapsed_ms}

# 2. 注册 Backend
from kernelgym.backend import register_backend
register_backend("my_backend", MyBackend)

# 3. 使用新 Backend
payload = {
    "backend": "my_backend",
    "kernel_code": "...",
    ...
}
```

### 9.2 添加新 Toolkit

```python
# 1. 实现 Toolkit 基类
class MyToolkit(Toolkit):
    name = "my_toolkit"

    def evaluate(self, task: Dict, backend: Backend, **kwargs) -> Dict:
        # 编译
        artifact = backend.compile(task["code"])
        if not artifact["compiled"]:
            return {"status": "failed", "error": "compilation failed"}

        # 加载和运行
        handle = backend.load(artifact)
        result = backend.run(handle, task["inputs"])

        # 清理
        backend.cleanup(handle)

        return {
            "status": "completed",
            "output": result["output"],
            "runtime": result["runtime"],
        }

# 2. 注册 Toolkit
from kernelgym.toolkit import register_toolkit
register_toolkit("my_toolkit", MyToolkit)
```

### 9.3 添加新 Workflow

```python
# 1. 实现 WorkflowController
class MyWorkflowController(WorkflowController):
    async def handle_request(self, input_data: Dict, scheduler: SchedulerAPI) -> Dict:
        # 创建任务规格
        task_spec = TaskSpec(
            kind="my_task",
            payload={
                **input_data,
                "toolkit": "my_toolkit",
                "backend_adapter": "my_backend",
            }
        )

        # 提交并等待
        task_id = await scheduler.submit(task_spec)
        result = await scheduler.wait(task_id)

        return result

# 2. 注册 Workflow
from kernelgym.workflow import register_workflow_controller
register_workflow_controller("my_workflow", MyWorkflowController)

# 3. 使用新 Workflow
response = await client.post(
    "http://localhost:10907/workflow/submit",
    json={
        "workflow": "my_workflow",
        "payload": {...}
    }
)
```

### 9.4 多节点部署扩展

```yaml
# 主节点配置
REDIS_HOST=0.0.0.0
REDIS_PORT=6379
API_HOST=0.0.0.0
API_PORT=10907

# Worker 节点配置
API_HOST=<main_node_ip>
API_PORT=10907
REDIS_HOST=<main_node_ip>
REDIS_PORT=6379
NODE_ID=worker-node-1
GPU_DEVICES=[0,1,2,3]
```

---

## 10. DR.Kernel 训练框架

### 10.1 框架概述

DR.Kernel 是基于 VERL 框架的强化学习训练系统，专门用于训练 GPU 内核生成模型。

### 10.2 核心技术创新

#### 10.2.1 TRLOO (Turn-level REINFORCE Leave-One-Out)

解决多轮 RL 中的优势估计偏差问题：

```python
def compute_multi_turn_rloo_outcome_advantage(
    token_level_rewards: torch.Tensor,
    eos_mask: torch.Tensor,
    loss_mask: torch.Tensor,
    turn_indices: torch.Tensor,
    index: np.ndarray,
    max_turns: int,
    gamma: float = 1.0,
):
    """
    Turn-aware REINFORCE Leave-one-out:
    计算使用相同 prompt、相同 turn、loss_mask == 1 的其他样本均值作为基线
    """
    # 计算回报
    returns = compute_multi_turn_returns(scores, gamma, max_turns)

    # 按 (prompt_index, turn_index) 分组
    for i in range(bsz):
        idx = (index[i], turn_indices[i].item())
        id2return[idx].append(returns[i])

    # 计算 LOO 基线
    for idx in id2return:
        id2mean[idx] = torch.mean(torch.tensor(id2return[idx]))

    # 计算优势
    for i in range(bsz):
        response_num = len(id2return[idx])
        if response_num > 1:
            advantages[i] = returns[i] * n / (n-1) - mean * n / (n-1)
        else:
            advantages[i] = returns[i]
```

#### 10.2.2 MRS (Multi-turn Rejection Sampling)

过滤低质量轨迹，减少奖励黑客和懒惰优化：

```python
# 配置示例
ROLLOUT_RS = "geometric"      # 拒绝采样策略
COVERAGE_RS = "turn"          # 覆盖率拒绝采样
```

#### 10.2.3 PR (Profiling-based Rewards)

使用性能分析信号提供更密集、更可靠的奖励：

```python
def compute_kernel_reward_batch(solution_strs, ground_truths, entry_points, **kwargs):
    """
    计算内核代码奖励值
    
    奖励维度：
    1. 编译成功奖励
    2. 正确性奖励
    3. 性能加速奖励
    4. 覆盖率奖励
    """
    results = client.compute_batch_rewards(tasks, ...)
    
    return [
        {
            "score": reward,
            "correctness": correctness,
            "speedup": speedup,
            "coverage": coverage,
        }
        for result in results
    ]
```

#### 10.2.4 Dual-Clip PPO

稳定训练，防止极端更新：

```python
def compute_policy_loss(
    old_log_prob, log_prob, advantages, eos_mask,
    cliprange_low, cliprange_high, clip_ratio_c=3.0, ...
):
    # 标准 PPO 裁剪
    pg_losses2 = -advantages * torch.clamp(ratio, 1.0 - cliprange_low, 1.0 + cliprange_high)

    # Dual-clip：仅在优势为负时激活
    pg_losses3 = -advantages * clip_ratio_c
    clip_pg_losses2 = torch.minimum(pg_losses3, clip_pg_losses1)
    pg_losses = torch.where(advantages < 0, clip_pg_losses2, clip_pg_losses1)
```

### 10.3 训练流程

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                        DR.Kernel Training Pipeline                           │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│  ┌──────────────────────────────────────────────────────────────────────┐   │
│  │ Phase 1: SFT Cold Start                                               │   │
│  │  ┌────────────────────────────────────────────────────────────────┐  │   │
│  │  │ - 使用高质量内核代码数据集                                      │  │   │
│  │  │ - 监督微调基础模型                                              │  │   │
│  │  │ - 获得初始代码生成能力                                          │  │   │
│  │  └────────────────────────────────────────────────────────────────┘  │   │
│  └──────────────────────────────────────────────────────────────────────┘   │
│                              │                                               │
│                              ▼                                               │
│  ┌──────────────────────────────────────────────────────────────────────┐   │
│  │ Phase 2: RL Training (TRLOO + MRS + PR + PRS)                        │   │
│  │  ┌────────────────────────────────────────────────────────────────┐  │   │
│  │  │ for epoch in range(num_epochs):                                │  │   │
│  │  │     # 1. 多轮 Rollout                                          │  │   │
│  │  │     trajectories = actor.generate(batch, max_turns=3)          │  │   │
│  │  │                                                                │  │   │
│  │  │     # 2. 奖励计算 (KernelGYM)                                  │  │   │
│  │  │     rewards = kernel_reward_fn(trajectories)                   │  │   │
│  │  │                                                                │  │   │
│  │  │     # 3. 拒绝采样过滤 (MRS)                                    │  │   │
│  │  │     filtered = rejection_sampling(trajectories, rewards)       │  │   │
│  │  │                                                                │  │   │
│  │  │     # 4. TRLOO 优势估计                                        │  │   │
│  │  │     advantages = compute_trloo_advantage(filtered)             │  │   │
│  │  │                                                                │  │   │
│  │  │     # 5. Dual-Clip PPO 更新                                    │  │   │
│  │  │     loss = compute_policy_loss(advantages, ...)                │  │   │
│  │  │     optimizer.step()                                           │  │   │
│  │  └────────────────────────────────────────────────────────────────┘  │   │
│  └──────────────────────────────────────────────────────────────────────┘   │
│                              │                                               │
│                              ▼                                               │
│  ┌──────────────────────────────────────────────────────────────────────┐   │
│  │ Phase 3: Evaluation                                                   │   │
│  │  ┌────────────────────────────────────────────────────────────────┐  │   │
│  │  │ - KernelBench 基准测试                                          │  │   │
│  │  │ - 多轮迭代优化评估                                              │  │   │
│  │  │ - 与基线模型对比                                                │  │   │
│  │  └────────────────────────────────────────────────────────────────┘  │   │
│  └──────────────────────────────────────────────────────────────────────┘   │
│                                                                              │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 10.4 奖励函数设计

```python
class KernelRewardManager:
    def compute_reward(self, result: Dict) -> float:
        """
        多维度奖励计算
        
        奖励组成：
        1. 编译奖励：compiled ? +0.1 : -0.1
        2. 正确性奖励：correctness ? +0.5 : -0.5
        3. 性能奖励：speedup > 1.0 ? +log(speedup) : -0.1
        4. 覆盖率奖励：coverage_score
        5. 诱饵内核惩罚：decoy_kernel ? -1.0 : 0
        """
        reward = 0.0

        # 编译奖励
        if result["compiled"]:
            reward += 0.1
        else:
            return -0.1  # 编译失败直接返回负奖励

        # 正确性奖励
        if result["correctness"]:
            reward += 0.5
        else:
            return -0.5  # 正确性失败

        # 性能奖励
        speedup = result.get("speedup", 0)
        if speedup > 1.0:
            reward += np.log(speedup)
        else:
            reward -= 0.1

        # 诱饵内核惩罚
        if result.get("decoy_kernel"):
            reward -= 1.0

        return reward
```

---

## 11. 部署与运维

### 11.1 单节点部署

```bash
# 1. 安装依赖
bash setup.sh

# 2. 自动配置
bash scripts/auto_configure.sh

# 3. 启动服务
./start_all_with_monitor.sh

# 4. 验证服务
curl http://localhost:10907/health
curl http://localhost:10907/workers/status
```

### 11.2 多节点部署

```bash
# 主节点
redis-server --bind 0.0.0.0
python -m kernelgym.server.api.server

# Worker 节点
export API_HOST=<main_node_ip>
export REDIS_HOST=<main_node_ip>
export NODE_ID=worker-node-1
export GPU_DEVICES=[0,1,2,3]
./start_worker_multinode.sh
```

### 11.3 配置管理

```python
# .env 配置文件
REDIS_HOST=localhost
REDIS_PORT=6379
API_HOST=0.0.0.0
API_PORT=10907
GPU_DEVICES=[0,1,2,3,4,5,6,7]
NODE_ID=node-1

# Worker 配置
WORKER_POOL_SIZE=1              # 每个 GPU 的 worker 进程数
MAX_TASKS_PER_WORKER=1          # 每个 worker 最大任务数
WORKER_QUEUE_WAIT_TIMEOUT_SEC=180
```

### 11.4 监控指标

```python
# 系统健康检查
GET /health
{
    "status": "healthy",
    "redis": "connected",
    "workers": 8,
    "queue_pending": 10
}

# Worker 状态
GET /workers/status
{
    "node-1_gpu_0": {
        "device": "cuda:0",
        "status": "online",
        "last_heartbeat": "2025-01-15T10:30:00",
        "tasks_processed": 150,
        "stats": {
            "tasks_completed": 145,
            "tasks_failed": 5,
            "average_processing_time": 2.5
        }
    }
}

# 队列状态
GET /queue/status
{
    "pending": 10,
    "pending_by_prefix": {"kernelgym": 10},
    "worker_queues": {"node-1_gpu_0": 2}
}
```

---

## 总结

KernelGYM 是一个设计精良的 GPU 内核评估和训练平台，其核心亮点包括：

1. **分层架构**：清晰的 Backend/Toolkit/Workflow 分层，支持灵活扩展
2. **子进程隔离**：CUDA 错误完全隔离，自动恢复机制保证系统稳定性
3. **分布式设计**：Redis 任务队列支持从单 GPU 到多节点的无缝扩展
4. **RL 集成**：与 VERL 框架深度集成，支持多轮强化学习训练
5. **技术创新**：TRLOO、MRS、PR、Dual-Clip PPO 等创新算法解决内核生成中的独特挑战

该项目为 GPU 内核生成的强化学习研究提供了完整的解决方案，具有重要的学术和工业价值。
