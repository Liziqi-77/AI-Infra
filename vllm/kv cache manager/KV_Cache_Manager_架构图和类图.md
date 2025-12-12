# KV Cache Manager 架构图和类图

## 1. 整体架构图

```mermaid
flowchart TB
    subgraph Client["推理引擎 (vLLM/SGLang)"]
        Engine["推理请求<br/>• 生成 KV Cache<br/>• 查询 KV Cache"]
    end

    subgraph KVCacheManager["KV Cache Manager"]
        direction TB
        
        subgraph API["公共 API 层"]
            Acquire["acquire()<br/>获取缓存数据"]
            Put["put()<br/>写入缓存数据"]
            Exists["exists()<br/>检查缓存存在"]
            Delete["delete()<br/>删除缓存"]
            Allocate["allocate_for()<br/>分配内存区域"]
        end

        subgraph Core["核心逻辑层"]
            AcquireImpl["_acquire_impl()<br/>• L1 优先查询<br/>• L2 补充查询<br/>• 双缓存策略"]
            PutImpl["put() 实现<br/>• 验证参数<br/>• L1 写入<br/>• L2 异步同步"]
            DoubleGet["_use_double_get()<br/>智能决策是否查询 L2"]
            ChunkKeys["cache_chunk_keys()<br/>分块处理长序列"]
        end

        subgraph L1["L1 缓存层 (本地内存)"]
            L1Cache["L1Cache<br/>• 快速访问<br/>• CPU/GPU 内存<br/>• 容量限制"]
            EvictionPolicy["驱逐策略<br/>• LRU<br/>• FIFO<br/>• S3FIFO"]
            Callback["回调机制<br/>• on_put<br/>• on_evict<br/>• on_hot_access"]
        end

        subgraph L2["L2 缓存层 (远程存储)"]
            L2Cache["L2Cache<br/>• 持久化存储<br/>• 大容量<br/>• 异步操作"]
            Connector["后端连接器<br/>• RocksDB<br/>• InfiniStore<br/>• HPKV<br/>• PRIS<br/>• EIC"]
            AsyncLoop["异步事件循环<br/>• 独立线程<br/>• 非阻塞<br/>• 超时控制"]
            KeyBuilder["键构建器<br/>• HexKeyBuilder<br/>• RawKeyBuilder<br/>• RollingHashKeyBuilder"]
        end

        subgraph Memory["内存管理层"]
            Allocator["TensorPoolAllocator<br/>• 内存池管理<br/>• Slab 分配<br/>• 引用计数"]
            Handle["KVCacheHandle<br/>• 零拷贝访问<br/>• 生命周期管理<br/>• Tensor 转换"]
            MR["MemoryRegion<br/>• 内存区域抽象<br/>• 引用计数<br/>• 序列化支持"]
        end

        subgraph Metrics["监控层"]
            MetricsCollector["KVCacheMetrics<br/>• L1 指标<br/>• L2 指标<br/>• 命中率统计<br/>• 延迟追踪"]
        end

        subgraph Meta["元数据服务"]
            MetaService["MetaService<br/>• Redis 后端<br/>• Placement 策略<br/>• 分布式协调"]
        end
    end

    subgraph Storage["存储后端"]
        RocksDB["RocksDB<br/>本地持久化"]
        InfiniStore["InfiniStore<br/>分布式 KV"]
        HPKV["HPKV<br/>高性能存储"]
        Other["其他后端..."]
    end

    Engine -->|"1. 查询/写入请求"| API
    API -->|"2. 路由到核心逻辑"| Core
    
    Core -->|"3a. 优先查询"| L1
    Core -->|"3b. 补充查询"| L2
    Core -->|"4. 决策逻辑"| DoubleGet
    
    L1 -->|"5. 内存分配"| Memory
    L2 -->|"6. 内存分配"| Memory
    L1 -->|"7. 驱逐触发"| EvictionPolicy
    EvictionPolicy -.->|"8. 回调同步"| L2
    
    L2 -->|"9. 异步操作"| AsyncLoop
    L2 -->|"10. 后端连接"| Connector
    Connector -->|"11. 存储操作"| Storage
    
    L2 -->|"12. 元数据查询"| Meta
    Meta --> MetaService
    
    Core -->|"13. 指标收集"| Metrics
    Memory -->|"14. 句柄管理"| Handle

    style KVCacheManager fill:#e1f5fe,stroke:#01579b,stroke-width:3px
    style L1 fill:#fff3e0,stroke:#e65100,stroke-width:2px
    style L2 fill:#e8f5e9,stroke:#1b5e20,stroke-width:2px
    style Memory fill:#fce4ec,stroke:#880e4f,stroke-width:2px
    style Metrics fill:#f3e5f5,stroke:#4a148c,stroke-width:2px
    style Core fill:#e0f2f1,stroke:#004d40,stroke-width:2px
```

---

## 2. 数据流图

```mermaid
sequenceDiagram
    autonumber
    participant Engine as 推理引擎
    participant Manager as KV Cache Manager
    participant L1 as L1 Cache
    participant L2 as L2 Cache
    participant Allocator as Memory Allocator
    participant Backend as 存储后端

    Note over Engine,Backend: 写入流程 (put)
    Engine->>Manager: put(prefix, query, kv_tensors)
    Manager->>Manager: 验证参数 (对齐、长度限制)
    
    alt L1 缓存启用
        Manager->>L1: put(prefix, query, tensors)
        L1->>Allocator: 分配内存 (如果需要)
        Allocator-->>L1: MemoryRegion[]
        L1->>L1: 更新缓存 + 驱逐策略
        L1-->>Manager: Status[int] (块数)
        
        alt 触发 L2 同步 (根据 ingestion_type)
            L1->>Manager: 回调 _l2_ingestion_callback()
            Manager->>L2: _l2_put(prefix, query, mr)
            alt 异步写入
                L2->>AsyncLoop: 提交异步任务
                AsyncLoop->>Backend: put(key, data)
                Backend-->>AsyncLoop: Status
                AsyncLoop-->>L2: Future 完成
            else 同步写入
                L2->>Backend: put(key, data)
                Backend-->>L2: Status
            end
        end
    else 仅 L2 缓存
        Manager->>L2: _l2_put(prefix, query, kv_tensors)
        L2->>Backend: put(key, data)
        Backend-->>L2: Status
        L2-->>Manager: Status
    end
    
    Manager-->>Engine: Status[int] (token 数)

    Note over Engine,Backend: 读取流程 (acquire)
    Engine->>Manager: acquire(prefix, query)
    Manager->>Manager: _acquire_impl(prefix, query)
    
    alt L1 缓存启用
        Manager->>L1: acquire(prefix, query)
        L1-->>Manager: Status[MemoryRegion[]]
        
        alt L1 部分命中
            Manager->>Manager: _use_double_get() 决策
            alt 需要查询 L2
                Manager->>Allocator: 分配缺失块的内存
                Allocator-->>Manager: MemoryRegion[]
                Manager->>L2: get(prefix_curr, tokens_curr, mrs)
                L2->>Backend: get(key, mr)
                Backend-->>L2: Status
                L2-->>Manager: Status[int] (块数)
                
                Manager->>L1: put(prefix_curr, tokens, mrs)
                Note over L1: 将 L2 数据写入 L1
            end
        end
    else 仅 L2 缓存
        Manager->>Allocator: allocate_for(prefix, query)
        Allocator-->>Manager: KVCacheHandle
        Manager->>L2: get(prefix, query, mrs)
        L2->>Backend: get(key, mr)
        Backend-->>L2: Status
        L2-->>Manager: Status
    end
    
    Manager-->>Engine: Status[(token_count, handle)]
    Engine->>Engine: 使用 handle.to_tensors()
    Engine->>Manager: handle.release()
```

---

## 3. 类图

```mermaid
classDiagram
    class KVCacheManager {
        <<abstract>>
        +config: KVCacheConfig
        +block_spec: KVCacheBlockSpec
        +block_layout: KVCacheBlockLayout
        +block_shape: Tuple[int, ...]
        +block_dtype: torch.dtype
        +block_ntokens: int
        +block_nbytes: int
        +acquire() Status[Tuple[int, KVCacheHandle]]
        +get() Status[int]
        +put() Status[int]
        +exists() Status[int]
        +delete() Status
        +allocate_for() Status[KVCacheHandle]
        +prefetch() None
        +register_kvcache() Status
        +flush() Status
        +close() None
        +cache_chunk_keys() Iterator
        +feature: KVCacheFeature
        +block_size: int
        +chunk_size: int
        +metrics: KVCacheMetrics
    }

    class BaseKVCacheManager {
        -_l1_cache: L1Cache | None
        -_l2_cache: L2Cache | None
        -_allocator: TensorPoolAllocator
        -_executor: Executor | None
        -_event_loop: asyncio.AbstractEventLoop | None
        -_thread: threading.Thread | None
        -_metrics: KVCacheMetrics
        -_ms: MetaService | None
        -_lock: threading.Lock
        -_infight_cv: threading.Condition
        -_l2_inflight_writes: int
        -_l2_inflight_quota: int
        -_chunk_size: int
        -_max_seq_len: int
        -_double_get_threshold: Tuple[int, float]
        -_l2_cache_per_token_timeout_ms: int
        +_acquire_impl() Status[Sequence[MemoryRegion]]
        +_get_impl() Status[int]
        +_exists_impl() Status[int]
        +_l2_put() Status
        +_l2_put_async() Status
        +_l2_put_sync() Status
        +_l2_ingestion_callback() Status
        +_use_double_get() bool
        +_release() None
    }

    class GroupAwareKVCacheManager {
        +process_group: dist.ProcessGroup
        +world_size: int
        +rank: int
        -_coll_tensor: torch.Tensor
        -_COLL_STATUS_ERROR: int
        -_COLL_STATUS_NOT_FOUND: int
        +_group_aware_acquire_impl() Status[Tuple[int, Sequence[MemoryRegion]]]
        +acquire() Status[Tuple[int, KVCacheHandle]]
        +get() Status[int]
    }

    class L1Cache {
        -_eviction_policy: BaseEvictionPolicy
        -allocator: TensorPoolAllocator
        -capacity_nbytes: int
        -block_spec: KVCacheBlockSpec
        -_cond_lock: ConditionalLock
        +put() Status[int]
        +acquire() Status[Sequence[MemoryRegion]]
        +exists() Status[int]
        +delete() Status
        +allocate() Status[Sequence[MemoryRegion]]
        +set_on_put_callback() None
        +set_on_evict_callback() None
        +set_on_hot_access_callback() None
        +__len__() int
    }

    class L2Cache {
        -_backend: Connector
        -_executor: Executor
        -key_builder: KeyBuilder
        -op_batch: int
        -block_spec: KVCacheBlockSpec
        +put() Status
        +get() Status
        +exists() Status[int]
        +delete() Status
        +prefetch() Status
        +open() Status
        +close() Status
        +register_slabs() Status
    }

    class TensorPoolAllocator {
        -slabs: List[torch.Tensor]
        -capacity_nbytes: int
        -_used_nbytes: int
        -device: str
        -pin_memory: bool
        +alloc() Status[Sequence[MemoryRegion]]
        +free() None
        +capacity_nbytes: int
        +_used_nbytes: int
    }

    class KVCacheHandle {
        <<abstract>>
        +memory_regions: Sequence[MemoryRegion]
        +memory_region_type: type[MemoryRegion]
        +to_tensors() Sequence[torch.Tensor]
        +release() None
        +truncate() None
        +__len__() int
    }

    class MemoryRegionKVCacheHandle {
        -_mrs: Sequence[MemoryRegion]
        -_block_dtype: torch.dtype
        -_block_shape: Tuple[int, ...]
        -_mr_type: type[MemoryRegion]
    }

    class GDRKVCacheHandle {
        -_mrs: Sequence[Sequence[MemoryRegion]]
        -_block_dtype: torch.dtype
        -_block_shape: Tuple[int, ...]
    }

    class KVCacheMetrics {
        +l1: L1CacheMetrics
        +l2: L2CacheMetrics
        +mgr: MetricRecorder
    }

    class L1CacheMetrics {
        +hit_count: Counter
        +miss_count: Counter
        +eviction_count: Counter
        +usage_bytes: Gauge
    }

    class L2CacheMetrics {
        +put_count: Counter
        +get_count: Counter
        +hit_count: Counter
        +miss_count: Counter
        +latency: Histogram
    }

    class MetaService {
        <<abstract>>
        +name: str
        +open() Status
        +close() Status
    }

    class RedisMetaService {
        -client: redis.Redis
        +open() Status
        +close() Status
    }

    class Connector {
        <<abstract>>
        +name: str
        +feature: ConnectorFeature
        +put() Status
        +get() Status
        +exists() Status
        +delete() Status
        +prefetch() None
        +register_slabs() Status
        +open() Status
        +close() Status
    }

    class BaseEvictionPolicy {
        <<abstract>>
        +name: str
        +capacity_nbytes: int
        +put() Status[int]
        +evict() None
        +__len__() int
        +__contains__() bool
    }

    class LRUEvictionPolicy {
        -_cache: OrderedDict
        -_access_order: deque
    }

    class FIFOEvictionPolicy {
        -_cache: OrderedDict
        -_queue: deque
    }

    class S3FIFOEvictionPolicy {
        -_s: OrderedDict
        -_m: OrderedDict
        -_g: OrderedDict
    }

    class KeyBuilder {
        <<abstract>>
        +block_size: int
        +signature: str
        +build_key() str | bytes
    }

    class RawKeyBuilder {
        +build_key() bytes
    }

    class HexKeyBuilder {
        +build_key() str
    }

    class MemoryRegion {
        <<abstract>>
        +slab: torch.Tensor
        +addr: int
        +length: int
        +capacity: int
        +ref_count: int
        +ref_up() None
        +ref_down() None
        +to_tensor() torch.Tensor
        +copy() MemoryRegion
    }

    class ManagedMemoryRegion {
        -allocator: TensorPoolAllocator
        -_block_nbytes: int
        -_is_sealed: bool
        -_prefix: KVCacheKeyTypes | None
        -_query: KVCacheKeyTypes | None
        +seal() None
        +pack_tokens() None
        +destroy_unsafe() None
    }

    class ExternalMemoryRegion {
        -on_release: Callable | None
    }

    KVCacheManager <|.. BaseKVCacheManager
    BaseKVCacheManager <|-- GroupAwareKVCacheManager
    BaseKVCacheManager *-- L1Cache : uses
    BaseKVCacheManager *-- L2Cache : uses
    BaseKVCacheManager *-- TensorPoolAllocator : uses
    BaseKVCacheManager *-- KVCacheMetrics : uses
    BaseKVCacheManager ..> MetaService : optional
    
    L1Cache *-- BaseEvictionPolicy : uses
    L1Cache *-- TensorPoolAllocator : uses
    BaseEvictionPolicy <|-- LRUEvictionPolicy
    BaseEvictionPolicy <|-- FIFOEvictionPolicy
    BaseEvictionPolicy <|-- S3FIFOEvictionPolicy
    
    L2Cache *-- Connector : uses
    L2Cache *-- KeyBuilder : uses
    KeyBuilder <|-- RawKeyBuilder
    KeyBuilder <|-- HexKeyBuilder
    
    BaseKVCacheManager ..> KVCacheHandle : returns
    KVCacheHandle <|-- MemoryRegionKVCacheHandle
    KVCacheHandle <|-- GDRKVCacheHandle
    
    MemoryRegionKVCacheHandle *-- MemoryRegion : contains
    MemoryRegion <|-- ManagedMemoryRegion
    MemoryRegion <|-- ExternalMemoryRegion
    
    TensorPoolAllocator *-- ManagedMemoryRegion : creates
    
    KVCacheMetrics *-- L1CacheMetrics
    KVCacheMetrics *-- L2CacheMetrics
    
    MetaService <|-- RedisMetaService
```

---

## 4. 组件交互图

```mermaid
graph LR
    subgraph Manager["BaseKVCacheManager"]
        API[API 接口]
        Core[核心逻辑]
    end

    subgraph L1["L1 Cache"]
        L1Impl[L1Cache 实现]
        Policy[驱逐策略]
        Callback[回调函数]
    end

    subgraph L2["L2 Cache"]
        L2Impl[L2Cache 实现]
        Conn[连接器]
        Async[异步循环]
    end

    subgraph Memory["Memory"]
        Alloc[分配器]
        Handle[句柄]
        MR[内存区域]
    end

    API -->|"1. 请求"| Core
    Core -->|"2. 查询"| L1Impl
    Core -->|"3. 查询"| L2Impl
    Core -->|"4. 分配"| Alloc
    
    L1Impl -->|"5. 策略"| Policy
    Policy -->|"6. 触发"| Callback
    Callback -.->|"7. 同步"| L2Impl
    
    L2Impl -->|"8. 操作"| Conn
    L2Impl -->|"9. 异步"| Async
    Async -->|"10. 执行"| Conn
    
    Alloc -->|"11. 创建"| MR
    MR -->|"12. 包装"| Handle
    Handle -->|"13. 返回"| API

    style Manager fill:#e1f5fe
    style L1 fill:#fff3e0
    style L2 fill:#e8f5e9
    style Memory fill:#fce4ec
```

---

## 5. 状态转换图

```mermaid
stateDiagram-v2
    [*] --> Initializing: 创建实例

    state Initializing {
        [*] --> InitBase: 调用父类 __init__
        InitBase --> InitComponents: 初始化组件变量
        InitComponents --> ConfigParams: 配置参数
        ConfigParams --> InitMetrics: 创建指标收集器
        InitMetrics --> InitAllocator: 创建内存分配器
        InitAllocator --> InitL1: 创建 L1 缓存
        InitL1 --> InitL2: 创建 L2 缓存
        InitL2 --> StartAsyncLoop: 启动异步循环
        StartAsyncLoop --> RegisterCallbacks: 注册回调
        RegisterCallbacks --> Ready: 初始化完成
    }

    Ready --> Operating: 开始使用

    state Operating {
        [*] --> Idle
        
        Idle --> Acquiring: acquire() 调用
        Idle --> Putting: put() 调用
        Idle --> Checking: exists() 调用
        Idle --> Deleting: delete() 调用
        
        Acquiring --> L1Query: 查询 L1
        L1Query --> L1Hit: L1 命中
        L1Query --> L1Miss: L1 未命中
        L1Miss --> L2Query: 查询 L2
        L2Query --> L2Hit: L2 命中
        L2Query --> L2Miss: L2 未命中
        L2Hit --> L1Update: 更新 L1
        L1Hit --> Idle
        L1Update --> Idle
        L2Miss --> Idle
        
        Putting --> L1Write: 写入 L1
        L1Write --> L1Full: L1 已满
        L1Full --> Evict: 执行驱逐
        Evict --> L2Sync: 同步到 L2
        L1Write --> L1Success: L1 成功
        L1Success --> L2Sync: 触发 L2 同步
        L2Sync --> AsyncWrite: 异步写入 L2
        AsyncWrite --> Idle
        
        Checking --> L1Check: 检查 L1
        L1Check --> L2Check: 检查 L2
        L2Check --> Idle
        
        Deleting --> L1Delete: 删除 L1
        L1Delete --> L2Delete: 删除 L2
        L2Delete --> Idle
    }

    Operating --> Flushing: flush() 调用
    Flushing --> WaitingAsync: 等待异步操作
    WaitingAsync --> AllDone: 所有操作完成
    AllDone --> Operating: 返回操作状态

    Operating --> Closing: close() 调用
    
    state Closing {
        [*] --> FlushPending: 刷新待处理操作
        FlushPending --> StopAsyncLoop: 停止异步循环
        StopAsyncLoop --> StopThread: 停止线程
        StopThread --> CloseL2: 关闭 L2
        CloseL2 --> DeleteL1: 删除 L1
        DeleteL1 --> Closed: 关闭完成
    }

    Closing --> [*]
```

---

## 6. 内存管理流程图

```mermaid
flowchart TD
    Start["请求内存分配"] --> CheckL1{"L1 缓存启用?"}
    
    CheckL1 -->|"是"| L1Alloc["L1Cache.allocate()"]
    CheckL1 -->|"否"| DirectAlloc["Allocator.alloc()"]
    
    L1Alloc --> CheckPool{"分配器有足够内存?"}
    DirectAlloc --> CheckPool
    
    CheckPool -->|"是"| AllocSuccess["分配成功<br/>返回 MemoryRegion[]"]
    CheckPool -->|"否"| EvictNeeded{"需要驱逐?"}
    
    EvictNeeded -->|"是"| Evict["执行驱逐策略<br/>释放内存"]
    EvictNeeded -->|"否"| AllocFail["分配失败<br/>返回 OOM"]
    
    Evict --> RetryAlloc["重试分配"]
    RetryAlloc --> CheckPool
    
    AllocSuccess --> CreateHandle["创建 KVCacheHandle<br/>包装 MemoryRegion[]"]
    CreateHandle --> RefUp["增加引用计数<br/>mr.ref_up()"]
    RefUp --> Return["返回 Handle"]
    
    Return --> Use["使用 Handle"]
    Use --> Release["调用 release()"]
    Release --> RefDown["减少引用计数<br/>mr.ref_down()"]
    RefDown --> CheckRef{"引用计数 = 0?"}
    
    CheckRef -->|"是"| Free["释放内存<br/>allocator.free()"]
    CheckRef -->|"否"| Keep["保留内存"]
    
    Free --> End["内存回收完成"]
    Keep --> End

    style AllocSuccess fill:#c8e6c9
    style AllocFail fill:#ffcdd2
    style Evict fill:#fff9c4
    style Free fill:#e1f5fe
```

---

## 7. 双缓存查询决策流程图

```mermaid
flowchart TD
    Start["acquire() 请求"] --> L1Query["查询 L1 缓存"]
    L1Query --> L1Result{"L1 结果"}
    
    L1Result -->|"完全命中"| ReturnL1["返回 L1 结果"]
    L1Result -->|"部分命中"| CalcMissing["计算缺失块数"]
    L1Result -->|"完全未命中"| CalcMissing
    
    CalcMissing --> CheckL2{"L2 启用?"}
    CheckL2 -->|"否"| ReturnL1
    
    CheckL2 -->|"是"| CheckThreshold["检查双缓存阈值"]
    CheckThreshold --> ThresholdType{"阈值类型"}
    
    ThresholdType -->|"仅数量"| CheckNum{"缺失块数 >= 阈值?"}
    ThresholdType -->|"数量+比例"| CheckBoth{"缺失块数 >= 数量阈值<br/>AND<br/>缺失比例 >= 比例阈值?"}
    
    CheckNum -->|"否"| ReturnL1
    CheckNum -->|"是"| QueryL2
    CheckBoth -->|"否"| ReturnL1
    CheckBoth -->|"是"| QueryL2
    
    QueryL2["查询 L2 缓存"] --> L2Result{"L2 结果"}
    L2Result -->|"命中"| Merge["合并 L1 + L2 结果"]
    L2Result -->|"未命中"| ReturnL1
    
    Merge --> UpdateL1["更新 L1 缓存"]
    UpdateL1 --> ReturnMerged["返回合并结果"]
    
    ReturnL1 --> End
    ReturnMerged --> End

    style QueryL2 fill:#fff3e0
    style Merge fill:#e8f5e9
    style ReturnL1 fill:#e1f5fe
    style ReturnMerged fill:#c8e6c9
```

---

## 8. L2 异步写入流程图

```mermaid
flowchart TD
    Start["put() 触发 L2 同步"] --> CheckQuota{"检查并发配额"}
    
    CheckQuota -->|"配额已满"| Denied["返回 DENIED<br/>拒绝写入"]
    CheckQuota -->|"配额可用"| IncCounter["增加 _l2_inflight_writes"]
    
    IncCounter --> SubmitAsync["提交异步任务<br/>asyncio.run_coroutine_threadsafe()"]
    SubmitAsync --> ReturnImmediate["立即返回成功<br/>Status.ok(token_count)"]
    
    ReturnImmediate --> AsyncExec["异步执行<br/>L2Cache.put()"]
    AsyncExec --> BackendOp["后端存储操作<br/>Connector.put()"]
    
    BackendOp --> Success{"操作成功?"}
    Success -->|"是"| DecCounter["减少 _l2_inflight_writes<br/>通知等待线程"]
    Success -->|"否"| LogError["记录错误日志"]
    
    LogError --> DecCounter
    DecCounter --> ReleaseMR["释放 MemoryRegion<br/>_release([value])"]
    ReleaseMR --> End["异步操作完成"]
    
    Denied --> End

    style SubmitAsync fill:#fff3e0
    style AsyncExec fill:#e8f5e9
    style BackendOp fill:#fce4ec
    style DecCounter fill:#e1f5fe
```

---

这些图表全面展示了 KV Cache Manager 的架构、类关系、数据流、状态转换和关键流程。

