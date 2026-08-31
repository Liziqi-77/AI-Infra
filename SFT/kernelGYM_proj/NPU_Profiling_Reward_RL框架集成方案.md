# NPU平台Profiling作为Reward的RL框架集成方案

## 1. 方案背景与目标

### 1.1 背景

Dr.Kernel框架在GPU内核生成任务中创新性地将profiling数据作为reward信号的一部分，主要包括：
- **覆盖率奖励**：基于自定义内核在总计算中的占比
- **时间覆盖率**：自定义内核的CUDA时间占总时间的比例
- **数量覆盖率**：自定义内核数量占总内核数量的比例

VERL原始框架作为通用LLM强化学习框架，**不内置profiling作为reward的功能**，需要针对特定任务进行扩展。

### 1.2 目标

在昇腾NPU平台上构建独立的RL训练框架，实现类似Dr.Kernel的profiling作为reward功能，用于训练NPU内核生成模型。

---

## 2. 整体架构设计

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                    NPU Profiling Reward RL Framework                         │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │                         RL Training Layer                              │  │
│  │  ┌─────────────────────────────────────────────────────────────────┐  │  │
│  │  │ PPO/GRPO Trainer                                                │  │  │
│  │  │ - 策略优化                                                       │  │  │
│  │  │ - 价值函数估计                                                   │  │  │
│  │  │ - 优势计算                                                       │  │  │
│  │  └─────────────────────────────────────────────────────────────────┘  │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
│                                      │                                       │
│                                      ▼                                       │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │                       Reward Computation Layer                         │  │
│  │  ┌─────────────────────────────────────────────────────────────────┐  │  │
│  │  │ NPU Reward Manager                                              │  │  │
│  │  │ ┌─────────────────┐ ┌─────────────────┐ ┌─────────────────────┐│  │  │
│  │  │ │ Correctness     │ │ Performance     │ │ Profiling Coverage  ││  │  │
│  │  │ │ Reward          │ │ Reward          │ │ Reward              ││  │  │
│  │  │ │ (正确性奖励)    │ │ (加速比奖励)    │ │ (覆盖率奖励)        ││  │  │
│  │  │ └─────────────────┘ └─────────────────┘ └─────────────────────┘│  │  │
│  │  └─────────────────────────────────────────────────────────────────┘  │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
│                                      │                                       │
│                                      ▼                                       │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │                       NPU Evaluation Layer                             │  │
│  │  ┌─────────────────────────────────────────────────────────────────┐  │  │
│  │  │ NPU Kernel Evaluator                                            │  │  │
│  │  │ ┌─────────────────┐ ┌─────────────────┐ ┌─────────────────────┐│  │  │
│  │  │ │ Compilation    │ │ Correctness     │ │ Profiling           ││  │  │
│  │  │ │ Service        │ │ Verification    │ │ Service             ││  │  │
│  │  │ │ (编译服务)     │ │ (正确性验证)    │ │ (性能分析服务)      ││  │  │
│  │  │ └─────────────────┘ └─────────────────┘ └─────────────────────┘│  │  │
│  │  └─────────────────────────────────────────────────────────────────┘  │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
│                                      │                                       │
│                                      ▼                                       │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │                       Hardware Abstraction Layer                       │  │
│  │  ┌─────────────────────────────────────────────────────────────────┐  │
│  │  │ torch_npu API                                                    │  │  │
│  │  │ - torch_npu.profiler                                             │  │  │
│  │  │ - torch_npu.npu.*                                                │  │  │
│  │  │ - AiCMetrics                                                     │  │  │
│  │  └─────────────────────────────────────────────────────────────────┘  │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
│                                                                              │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 3. 需要实现的核心模块

### 3.1 模块总览

| 模块编号 | 模块名称 | 功能描述 | 优先级 |
|----------|----------|----------|--------|
| M1 | NPU Profiling Service | NPU性能数据采集服务 | P0 |
| M2 | NPU Kernel Evaluator | NPU内核评估服务 | P0 |
| M3 | NPU Reward Manager | 奖励计算与管理 | P0 |
| M4 | NPU Worker Pool | 分布式Worker池 | P1 |
| M5 | NPU Task Queue | 任务队列管理 | P1 |
| M6 | NPU API Server | REST API服务 | P1 |
| M7 | NPU Error Handler | 错误处理与恢复 | P2 |
| M8 | NPU Metrics Aggregator | 指标聚合与存储 | P2 |

### 3.2 模块详细说明

---

## 4. 模块M1：NPU Profiling Service

### 4.1 功能职责

- 封装torch_npu.profiler API
- 采集NPU内核执行的性能数据
- 提取AI Core性能计数器数据
- 生成标准化的profiling报告

### 4.2 核心接口

```
NPUProfilingService
├── start_profiling(config: ProfilingConfig)
├── stop_profiling() -> ProfilingResult
├── get_kernel_metrics() -> List[KernelMetric]
├── get_ai_core_counters() -> Dict[str, float]
├── compute_coverage(custom_kernels: List[str]) -> CoverageResult
└── export_trace(output_path: str)
```

### 4.3 数据结构

```
ProfilingConfig:
  - activities: [CPU, NPU]
  - profiler_level: Level0|Level1|Level2|Level3
  - ai_core_metrics: [MAC_FP16, MAC_INT8, MEMORY_BANDWIDTH, ...]
  - record_shapes: bool
  - profile_memory: bool

ProfilingResult:
  - kernels: List[KernelMetric]
  - total_npu_time_us: float
  - total_cpu_time_us: float
  - memory_stats: MemoryStats
  - ai_core_metrics: Dict[str, float]

KernelMetric:
  - name: str
  - npu_time_us: float
  - cpu_time_us: float
  - count: int
  - memory_usage_bytes: int

CoverageResult:
  - num_custom_kernels: int
  - num_total_kernels: int
  - custom_kernel_npu_time_us: float
  - total_npu_time_us: float
  - time_coverage: float  # 时间覆盖率
  - number_coverage: float  # 数量覆盖率
```

### 4.4 实现要点

1. **Profiler级别选择**
   - Level0: 不采集
   - Level1: 用户级
   - Level2: 设备级（推荐）
   - Level3: 全量采集

2. **AI Core计数器**
   - MAC_FP16/MAC_INT8: 计算吞吐量
   - MEMORY_BANDWIDTH: 内存带宽利用率
   - ICACHE_MISS/DCACHE_MISS: 缓存效率

3. **覆盖率计算**
   - 支持内核名称模糊匹配
   - 支持正则表达式匹配
   - 支持自定义匹配函数

---

## 5. 模块M2：NPU Kernel Evaluator

### 5.1 功能职责

- 编译NPU内核代码（Ascend C / torch_npu扩展）
- 加载编译后的内核模块
- 执行内核并验证正确性
- 测量内核性能（预热+多次试验）
- 集成profiling采集

### 5.2 核心接口

```
NPUKernelEvaluator
├── compile(kernel_code: str, entry_point: str) -> CompileResult
├── load(compile_artifact: CompileResult) -> KernelHandle
├── run(handle: KernelHandle, inputs: Dict) -> RunResult
├── verify_correctness(output: Tensor, reference: Tensor) -> bool
├── measure_performance(handle: KernelHandle, inputs: Dict, config: TimingConfig) -> TimingResult
└── evaluate_full(task: EvaluationTask) -> EvaluationResult
```

### 5.3 数据结构

```
EvaluationTask:
  - reference_code: str  # 参考实现
  - kernel_code: str     # 生成的内核代码
  - entry_point: str     # 入口函数名
  - test_inputs: List[Dict]  # 测试用例
  - timing_config: TimingConfig
  - profiling_config: ProfilingConfig

EvaluationResult:
  - compiled: bool
  - correctness: bool
  - decoy_kernel: bool  # 是否为诱饵内核
  - kernel_runtime_ms: float
  - reference_runtime_ms: float
  - speedup: float
  - profiling_result: ProfilingResult
  - coverage_result: CoverageResult
  - error_message: str
  - status: Status

TimingConfig:
  - num_warmup: int
  - num_trials: int
  - enable_profiling: bool
```

### 5.4 实现要点

1. **编译流程**
   - 支持Ascend C编译（ncc编译器）
   - 支持torch_npu扩展编译
   - 支持Triton-like DSL编译（需自定义）

2. **正确性验证**
   - 支持多测试用例
   - 支持自定义容差（rtol/atol）
   - 支持数值范围检查

3. **诱饵内核检测**
   - 检测是否直接调用参考实现
   - 检测是否使用torch原生算子
   - 检测是否为空实现

---

## 6. 模块M3：NPU Reward Manager

### 6.1 功能职责

- 接收评估结果
- 计算多维度奖励
- 支持多种奖励函数
- 管理奖励策略配置

### 6.2 核心接口

```
NPURewardManager
├── compute_reward(evaluation_result: EvaluationResult) -> RewardResult
├── compute_coverage_reward(profiling_result: ProfilingResult) -> CoverageReward
├── set_reward_policy(policy: RewardPolicy)
├── get_reward_statistics() -> RewardStatistics
└── register_custom_reward_function(name: str, func: Callable)
```

### 6.3 数据结构

```
RewardPolicy:
  - correctness_weight: float      # 正确性奖励权重
  - performance_weight: float      # 性能奖励权重
  - coverage_weight: float         # 覆盖率奖励权重
  - coverage_type: time_coverage | number_coverage
  - speedup_thresholds: Dict[float, float]  # 加速比阈值到奖励的映射
  - penalties:
      - compilation_fail: float
      - correctness_fail: float
      - perf_degrade: float
      - decoy_kernel: float

RewardResult:
  - reward: float              # 最终奖励值
  - score: float               # 分数（与reward相同）
  - correctness_reward: float  # 正确性奖励分量
  - performance_reward: float  # 性能奖励分量
  - coverage_reward: float     # 覆盖率奖励分量
  - speedup: float
  - compiled: bool
  - correctness: bool
  - coverage: float
  - profiling_metrics: Dict    # 详细profiling指标
```

### 6.4 奖励函数设计

#### 6.4.1 基础奖励函数

```
calculate_reward_basic(result):
    if not compiled:
        return compilation_fail_penalty
    
    if not correctness:
        return correctness_fail_penalty
    
    if speedup >= 3.0:
        return 1.0
    elif speedup >= 2.0:
        return 0.8
    elif speedup >= 1.5:
        return 0.6
    elif speedup >= 1.2:
        return 0.4
    elif speedup >= 1.0:
        return 0.2
    else:
        return perf_degrade_penalty
```

#### 6.4.2 加权奖励函数（含Profiling Coverage）

```
calculate_reward_weighted(result, policy):
    # 基础奖励
    reward = policy.correctness_weight * correctness
    reward += policy.performance_weight * (speedup >= 1.0 + eps)
    
    # 覆盖率奖励
    if correctness and policy.coverage_weight > 0:
        coverage = compute_coverage(result.profiling_result)
        reward += policy.coverage_weight * coverage
    
    return reward
```

#### 6.4.3 Speedup直接奖励函数

```
calculate_reward_speedup(result, policy):
    reward = policy.correctness_weight * correctness
    
    # 限制speedup奖励范围
    bounded_speedup = clamp(speedup, 
                            policy.speedup_lower_bound, 
                            policy.speedup_upper_bound)
    reward += policy.performance_weight * bounded_speedup
    
    # 覆盖率奖励
    if correctness:
        coverage = compute_coverage(result.profiling_result)
        reward += policy.coverage_weight * coverage
    
    return reward
```

### 6.5 覆盖率计算

```
compute_coverage(profiling_result, custom_kernel_names):
    # 时间覆盖率
    custom_npu_time = sum(
        kernel.npu_time_us 
        for kernel in profiling_result.kernels
        if matches_custom_kernel(kernel.name, custom_kernel_names)
    )
    time_coverage = custom_npu_time / profiling_result.total_npu_time_us
    
    # 数量覆盖率
    custom_count = sum(
        1 for kernel in profiling_result.kernels
        if matches_custom_kernel(kernel.name, custom_kernel_names)
    )
    number_coverage = custom_count / len(profiling_result.kernels)
    
    return CoverageResult(
        time_coverage=time_coverage,
        number_coverage=number_coverage,
        ...
    )
```

---

## 7. 模块M4：NPU Worker Pool

### 7.1 功能职责

- 管理多个NPU Worker进程
- 实现任务分发与结果收集
- 支持CUDA/NPU错误隔离
- 实现Worker自动重启

### 7.2 核心接口

```
NPUWorkerPool
├── initialize(num_workers: int, devices: List[int])
├── submit_task(task: Task) -> Future[Result]
├── get_worker_status() -> List[WorkerStatus]
├── restart_worker(worker_id: str)
└── shutdown()
```

### 7.3 架构设计

```
┌─────────────────────────────────────────────────────────────────┐
│                      NPU Worker Pool                             │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │                    Main Process                           │   │
│  │  ┌────────────────────────────────────────────────────┐  │   │
│  │  │ Task Queue Manager                                 │  │   │
│  │  │ - 任务分发                                          │  │   │
│  │  │ - 结果收集                                          │  │   │
│  │  │ - Worker状态监控                                    │  │   │
│  │  └────────────────────────────────────────────────────┘  │   │
│  └──────────────────────────────────────────────────────────┘   │
│                              │                                   │
│              ┌───────────────┼───────────────┐                  │
│              ▼               ▼               ▼                  │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐          │
│  │  Worker 0    │  │  Worker 1    │  │  Worker N    │          │
│  │  (NPU:0)     │  │  (NPU:1)     │  │  (NPU:N)     │          │
│  │              │  │              │  │              │          │
│  │ ┌──────────┐ │  │ ┌──────────┐ │  │ ┌──────────┐ │          │
│  │ │ Evaluator│ │  │ │ Evaluator│ │  │ │ Evaluator│ │          │
│  │ │ Profiler │ │  │ │ Profiler │ │  │ │ Profiler │ │          │
│  │ └──────────┘ │  │ └──────────┘ │  │ └──────────┘ │          │
│  │              │  │              │  │              │          │
│  │ Isolated     │  │ Isolated     │  │ Isolated     │          │
│  │ NPU Context  │  │ NPU Context  │  │ NPU Context  │          │
│  └──────────────┘  └──────────────┘  └──────────────┘          │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### 7.4 错误隔离机制

```
Worker执行流程:
1. 接收任务
2. 初始化NPU上下文
3. 执行评估
4. 捕获NPU错误
   - 如果是NPU错误:
     - 标记worker_exiting=True
     - 执行强力清理
     - 退出进程
   - 如果是其他错误:
     - 返回错误结果
     - 继续处理下一个任务
5. 返回结果

主进程处理:
1. 检测worker_exiting标志
2. 标记Worker为dead
3. 启动新Worker替代
4. 将任务重新入队
```

---

## 8. 模块M5：NPU Task Queue

### 8.1 功能职责

- 任务提交与存储
- 优先级队列管理
- 任务状态跟踪
- 结果缓存

### 8.2 核心接口

```
NPUTaskQueue
├── submit(task: Task, priority: Priority) -> task_id
├── get_status(task_id: str) -> TaskStatus
├── get_result(task_id: str) -> Result
├── cancel(task_id: str) -> bool
├── list_pending() -> List[Task]
└── clear_completed()
```

### 8.3 任务状态机

```
┌─────────┐    submit    ┌─────────┐    pick_up    ┌────────────┐
│ CREATED │─────────────▶│ PENDING │──────────────▶│ PROCESSING │
└─────────┘              └─────────┘               └──────┬─────┘
                                                          │
                         ┌────────────────────────────────┼────────────────────────┐
                         │                                │                        │
                         ▼                                ▼                        ▼
                   ┌───────────┐                   ┌──────────┐            ┌─────────┐
                   │ COMPLETED │                   │  FAILED  │            │ TIMEOUT │
                   └───────────┘                   └──────────┘            └─────────┘
                         │                                │                        │
                         └────────────────────────────────┴────────────────────────┘
                                                          │
                                                          ▼
                                                   ┌──────────┐
                                                   │ CACHED   │
                                                   └──────────┘
```

---

## 9. 模块M6：NPU API Server

### 9.1 功能职责

- 提供REST API接口
- 接收评估请求
- 返回评估结果
- 支持异步任务模式

### 9.2 API端点设计

```
POST /evaluate
  - 提交评估任务（同步模式）
  - 请求体: EvaluationRequest
  - 响应: EvaluationResult

POST /evaluate/async
  - 提交评估任务（异步模式）
  - 请求体: EvaluationRequest
  - 响应: { task_id: str }

GET /status/{task_id}
  - 查询任务状态
  - 响应: TaskStatus

GET /results/{task_id}
  - 获取任务结果
  - 响应: EvaluationResult

POST /evaluate/batch
  - 批量提交评估任务
  - 请求体: List[EvaluationRequest]
  - 响应: List[EvaluationResult]

GET /health
  - 健康检查
  - 响应: { status: str, workers: int, queue_size: int }

GET /metrics
  - 获取系统指标
  - 响应: SystemMetrics
```

### 9.3 请求/响应模型

```
EvaluationRequest:
  - task_id: str (optional)
  - reference_code: str
  - kernel_code: str
  - entry_point: str
  - backend: triton | ascend_c | torch_npu
  - num_correct_trials: int
  - num_perf_trials: int
  - timeout: int
  - enable_profiling: bool
  - detect_decoy_kernel: bool

EvaluationResponse:
  - task_id: str
  - status: str
  - compiled: bool
  - correctness: bool
  - speedup: float
  - reward: float
  - profiling: ProfilingResult
  - coverage: CoverageResult
  - error_message: str
```

---

## 10. 模块M7：NPU Error Handler

### 10.1 功能职责

- 错误分类与识别
- 错误恢复策略
- 错误日志记录
- 错误统计报告

### 10.2 错误分类

```
ErrorType:
├── CompilationError
│   ├── SyntaxError
│   ├── TypeError
│   └── LinkerError
├── RuntimeError
│   ├── NPUOutOfMemory
│   ├── NPUKernelLaunchFailed
│   └── NPUDeviceLost
├── CorrectnessError
│   ├── OutputMismatch
│   ├── ShapeMismatch
│   └── DtypeMismatch
├── TimeoutError
│   ├── CompilationTimeout
│   ├── ExecutionTimeout
│   └── ProfilingTimeout
└── SystemError
    ├── WorkerCrash
    ├── QueueFull
    └── NetworkError
```

### 10.3 恢复策略

```
RecoveryStrategy:
├── Retry(retry_count: int, backoff: float)
├── RestartWorker()
├── FallbackToCPU()
├── SkipAndContinue()
└── Abort()
```

---

## 11. 模块M8：NPU Metrics Aggregator

### 11.1 功能职责

- 聚合多次评估的指标
- 生成训练统计报告
- 支持实时监控
- 数据持久化存储

### 11.2 核心指标

```
TrainingMetrics:
├── EpisodeMetrics
│   ├── total_episodes: int
│   ├── successful_episodes: int
│   ├── average_reward: float
│   └── average_speedup: float
├── ProfilingMetrics
│   ├── average_coverage: float
│   ├── average_custom_kernel_count: int
│   └── average_npu_time_us: float
├── PerformanceMetrics
│   ├── evaluations_per_second: float
│   ├── average_latency_ms: float
│   └── queue_wait_time_ms: float
└── ResourceMetrics
    ├── npu_utilization: float
    ├── memory_usage_mb: float
    └── worker_count: int
```

---

## 12. 模块依赖关系

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                          模块依赖关系图                                       │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│                          ┌──────────────────┐                               │
│                          │  M6: API Server  │                               │
│                          └────────┬─────────┘                               │
│                                   │                                          │
│                                   ▼                                          │
│                    ┌──────────────────────────────┐                         │
│                    │      M5: Task Queue          │                         │
│                    └──────────────┬───────────────┘                         │
│                                   │                                          │
│                                   ▼                                          │
│                    ┌──────────────────────────────┐                         │
│                    │      M4: Worker Pool         │                         │
│                    └──────────────┬───────────────┘                         │
│                                   │                                          │
│              ┌────────────────────┼────────────────────┐                    │
│              │                    │                    │                    │
│              ▼                    ▼                    ▼                    │
│  ┌───────────────────┐ ┌───────────────────┐ ┌───────────────────┐         │
│  │ M2: Kernel        │ │ M1: Profiling     │ │ M7: Error         │         │
│  │     Evaluator     │ │     Service       │ │     Handler       │         │
│  └─────────┬─────────┘ └─────────┬─────────┘ └───────────────────┘         │
│            │                     │                                          │
│            └──────────┬──────────┘                                          │
│                       │                                                     │
│                       ▼                                                     │
│            ┌───────────────────┐                                            │
│            │ M3: Reward        │                                            │
│            │     Manager       │                                            │
│            └─────────┬─────────┘                                            │
│                      │                                                      │
│                      ▼                                                      │
│            ┌───────────────────┐                                            │
│            │ M8: Metrics       │                                            │
│            │     Aggregator    │                                            │
│            └───────────────────┘                                            │
│                                                                              │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 13. 实现优先级与路线图

### 13.1 Phase 1: 核心功能（P0）

```
Week 1-2:
├── M1: NPU Profiling Service
│   ├── torch_npu.profiler封装
│   ├── 基础指标提取
│   └── 覆盖率计算
└── M2: NPU Kernel Evaluator
    ├── 编译流程
    ├── 正确性验证
    └── 性能测量

Week 3-4:
├── M3: NPU Reward Manager
│   ├── 奖励函数实现
│   ├── 覆盖率奖励
│   └── 奖励策略配置
└── 集成测试
```

### 13.2 Phase 2: 分布式支持（P1）

```
Week 5-6:
├── M4: NPU Worker Pool
│   ├── 进程管理
│   ├── 错误隔离
│   └── 自动重启
├── M5: NPU Task Queue
│   ├── Redis集成
│   └── 优先级队列
└── M6: NPU API Server
    ├── FastAPI实现
    └── 异步任务支持
```

### 13.3 Phase 3: 生产就绪（P2）

```
Week 7-8:
├── M7: NPU Error Handler
│   ├── 错误分类
│   └── 恢复策略
├── M8: NPU Metrics Aggregator
│   ├── 指标聚合
│   └── 数据持久化
└── 文档与测试
    ├── API文档
    ├── 使用指南
    └── 性能测试
```

---

## 14. 与现有框架的集成方式

### 14.1 与VERL集成

```
# VERL自定义Reward函数
from npu_reward_framework import NPURewardManager

class NPUKernelRewardFunction:
    def __init__(self, config):
        self.reward_manager = NPURewardManager(config)
        self.evaluator = NPUKernelEvaluator(config)
    
    def __call__(self, prompts, responses, **kwargs):
        results = []
        for prompt, response in zip(prompts, responses):
            # 提取内核代码
            kernel_code = extract_kernel_code(response)
            reference_code = extract_reference_code(prompt)
            
            # 执行评估
            eval_result = self.evaluator.evaluate_full(
                reference_code=reference_code,
                kernel_code=kernel_code,
                enable_profiling=True
            )
            
            # 计算奖励
            reward = self.reward_manager.compute_reward(eval_result)
            results.append(reward)
        
        return results

# VERL配置
config = {
    "reward_function": NPUKernelRewardFunction(reward_config),
    ...
}
```

### 14.2 与SLIME集成

```
# SLIME自定义Reward Manager
from slime.rollout import RolloutManager
from npu_reward_framework import NPUKernelEvaluator, NPURewardManager

class NPURolloutManager(RolloutManager):
    def __init__(self, args):
        super().__init__(args)
        self.evaluator = NPUKernelEvaluator(args.npu_config)
        self.reward_manager = NPURewardManager(args.reward_config)
    
    async def compute_reward(self, batch):
        # 并行评估
        eval_tasks = [
            self.evaluator.evaluate_full_async(task)
            for task in batch
        ]
        eval_results = await asyncio.gather(*eval_tasks)
        
        # 计算奖励
        rewards = [
            self.reward_manager.compute_reward(result)
            for result in eval_results
        ]
        
        return rewards
```

---

## 15. 关键技术挑战与解决方案

### 15.1 NPU Profiling精度

| 挑战 | 解决方案 |
|------|----------|
| NPU Event计时精度不如CUDA | 使用多次试验取平均，增加预热次数 |
| AI Core计数器访问开销 | 选择性采集关键指标，避免全量采集 |
| Profiling影响实际性能 | 分离profiling运行和性能测量运行 |

### 15.2 内核编译与加载

| 挑战 | 解决方案 |
|------|----------|
| Ascend C编译复杂 | 提供标准编译模板，封装编译流程 |
| 动态加载受限 | 预编译内核库，运行时链接 |
| 编译时间长 | 编译缓存，增量编译 |

### 15.3 错误隔离

| 挑战 | 解决方案 |
|------|----------|
| NPU错误影响全局 | 子进程隔离，独立NPU上下文 |
| 错误恢复慢 | 快速重启机制，预热Worker池 |
| 错误分类困难 | 建立错误模式库，自动分类 |

---

## 16. 总结

本方案设计了一个完整的NPU平台Profiling作为Reward的RL框架，核心模块包括：

1. **NPU Profiling Service (M1)**：封装torch_npu.profiler，采集性能数据
2. **NPU Kernel Evaluator (M2)**：编译、加载、验证、测量NPU内核
3. **NPU Reward Manager (M3)**：计算多维度奖励，支持覆盖率奖励
4. **NPU Worker Pool (M4)**：分布式Worker管理，错误隔离
5. **NPU Task Queue (M5)**：任务队列管理
6. **NPU API Server (M6)**：REST API服务
7. **NPU Error Handler (M7)**：错误处理与恢复
8. **NPU Metrics Aggregator (M8)**：指标聚合与存储

该框架可与VERL或SLIME等现有RL框架无缝集成，为NPU内核生成模型训练提供完整的解决方案。
