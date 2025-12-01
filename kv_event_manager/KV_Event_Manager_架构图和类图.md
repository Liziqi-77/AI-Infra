# KV Event Manager 架构图和类图

## 1. 整体架构图

```mermaid
flowchart TB
    subgraph K8s["Kubernetes 集群"]
        subgraph vLLMPods["vLLM Pods"]
            Pod1["vLLM Pod 1<br/>IP: 10.0.0.1<br/>:5557 (PUB)<br/>:5558 (ROUTER)"]
            Pod2["vLLM Pod 2<br/>IP: 10.0.0.2<br/>:5557 (PUB)<br/>:5558 (ROUTER)"]
            PodN["vLLM Pod N<br/>IP: 10.0.0.N<br/>:5557 (PUB)<br/>:5558 (ROUTER)"]
        end
        
        Informer["K8s Pod Informer<br/>监听 Pod 事件"]
    end
    
    subgraph Gateway["AIBrix Gateway"]
        subgraph KVEventMgr["KV Event Manager"]
            Manager["Manager<br/>• 订阅管理<br/>• 生命周期控制"]
            Subscribers["Subscribers Map<br/>podKey → ZMQClient"]
        end
        
        subgraph ZMQClients["ZMQ Client Pool"]
            Client1["ZMQClient 1<br/>SUB + DEALER"]
            Client2["ZMQClient 2<br/>SUB + DEALER"]
            ClientN["ZMQClient N<br/>SUB + DEALER"]
        end
        
        subgraph Handlers["Event Handlers"]
            Handler1["eventHandler 1"]
            Handler2["eventHandler 2"]
            HandlerN["eventHandler N"]
        end
        
        subgraph SyncLayer["Sync Layer"]
            SyncProvider["SyncIndexProvider"]
            SyncIndexer["SyncPrefixHashTable<br/>• 前缀哈希存储<br/>• 驱逐管理"]
        end
        
        subgraph Routing["Routing Algorithm"]
            PrefixRouter["Prefix Cache Router<br/>MatchPrefix()"]
        end
    end
    
    %% K8s 事件流
    Informer -->|"OnPodAdd<br/>OnPodUpdate<br/>OnPodDelete"| Manager
    
    %% Manager 管理连接
    Manager -->|"管理"| Subscribers
    Subscribers -->|"存储"| Client1
    Subscribers -->|"存储"| Client2
    Subscribers -->|"存储"| ClientN
    
    %% ZMQ 连接
    Pod1 -.->|"ZMQ SUB"| Client1
    Pod2 -.->|"ZMQ SUB"| Client2
    PodN -.->|"ZMQ SUB"| ClientN
    
    %% Handler 绑定
    Client1 -->|"回调"| Handler1
    Client2 -->|"回调"| Handler2
    ClientN -->|"回调"| HandlerN
    
    %% 同步到 Indexer
    Handler1 -->|"ProcessBlockStored<br/>ProcessBlockRemoved"| SyncProvider
    Handler2 -->|"ProcessBlockStored<br/>ProcessBlockRemoved"| SyncProvider
    HandlerN -->|"ProcessBlockStored<br/>ProcessBlockRemoved"| SyncProvider
    SyncProvider -->|"委托"| SyncIndexer
    
    %% 路由查询
    PrefixRouter -->|"MatchPrefix()"| SyncIndexer
    
    style Manager fill:#e1f5fe
    style SyncIndexer fill:#fff3e0
    style PrefixRouter fill:#e8f5e9
```

---

## 2. 数据流图

```mermaid
flowchart LR
    subgraph vLLM["vLLM 推理引擎"]
        KVCache["KV Cache<br/>存储/驱逐"]
        Publisher["ZMQ Publisher<br/>:5557"]
    end
    
    subgraph Transport["ZMQ 传输层"]
        direction TB
        MSG["MessagePack 消息<br/>[topic, seq, payload]"]
    end
    
    subgraph Gateway["AIBrix Gateway"]
        subgraph Receive["接收层"]
            SUB["SUB Socket<br/>订阅消息"]
            Decoder["MessagePack<br/>Decoder"]
        end
        
        subgraph Process["处理层"]
            Handler["eventHandler<br/>HandleEvent()"]
            Convert["Token 转换<br/>[]int32 → []byte"]
        end
        
        subgraph Store["存储层"]
            Indexer["SyncPrefixHashTable"]
            PrefixMap["prefixMap<br/>hash → pods"]
            HashMapping["hashMapping<br/>engine → aibrix"]
        end
    end
    
    KVCache -->|"Block 变更"| Publisher
    Publisher -->|"ZMQ PUB"| MSG
    MSG -->|"ZMQ SUB"| SUB
    SUB -->|"bytes"| Decoder
    Decoder -->|"KVEvent"| Handler
    Handler -->|"转换"| Convert
    Convert -->|"同步"| Indexer
    Indexer -->|"存储前缀"| PrefixMap
    Indexer -->|"存储映射"| HashMapping
    
    style KVCache fill:#ffebee
    style Indexer fill:#e3f2fd
```

---

## 3. 类图

```mermaid
classDiagram
    class Manager {
        -podProvider PodProvider
        -syncProvider SyncIndexProvider
        -subscribers SyncMap~string, *ZMQClient~
        -enabled bool
        -ctx context.Context
        -cancel context.CancelFunc
        -mu sync.RWMutex
        -stopped bool
        
        +NewManager(podProvider, syncProvider) *Manager
        +Start() error
        +Stop()
        +OnPodAdd(pod *v1.Pod)
        +OnPodUpdate(oldPod, newPod *v1.Pod)
        +OnPodDelete(pod *v1.Pod)
        -subscribeToPod(ctx, podKey, podInfo) error
        -unsubscribeFromPod(podKey)
    }
    
    class PodProvider {
        <<interface>>
        +GetPod(ctx, podKey) (*PodInfo, bool)
        +RangePods(ctx, f func) error
    }
    
    class SyncIndexProvider {
        <<interface>>
        +GetSyncIndexer(ctx) (SyncIndexer, error)
    }
    
    class SyncIndexer {
        <<interface>>
        +ProcessBlockStored(ctx, event) error
        +ProcessBlockRemoved(ctx, event) error
        +RemovePrefix(ctx, modelName, loraID, podKey) error
    }
    
    class PodInfo {
        +Name string
        +Namespace string
        +PodIP string
        +ModelName string
        +Labels map~string~string
        +Models []string
    }
    
    class eventHandler {
        -manager *Manager
        -podKey string
        -modelName string
        -loraID int64
        
        +HandleEvent(event KVEvent) error
        -handleBlockStored(ctx, event) error
        -handleBlockRemoved(ctx, event) error
        -handleAllBlocksCleared(ctx, event) error
    }
    
    class ZMQClient {
        -config *ZMQClientConfig
        -subSocket *zmq.Socket
        -replaySocket *zmq.Socket
        -eventHandler EventHandler
        -connected bool
        -lastSeq int64
        -reconnectDelay time.Duration
        -ctx context.Context
        -cancel context.CancelFunc
        -wg sync.WaitGroup
        -metrics *ZMQClientMetrics
        
        +NewZMQClient(config, handler) *ZMQClient
        +Connect() error
        +Start() error
        +Stop()
        +IsConnected() bool
        +GetLastSequence() int64
        -consumeEventsWithReconnect()
        -handleReconnect()
        -consumeEvents() error
        -processMessage() error
        -requestReplay(fromSeq) error
        -markDisconnected()
        -cleanupSocketsLocked()
    }
    
    class EventHandler {
        <<interface>>
        +HandleEvent(event KVEvent) error
    }
    
    class ZMQClientConfig {
        +PodKey string
        +PodIP string
        +ModelName string
        +PubPort int
        +RouterPort int
        +PollTimeout time.Duration
        +ReplayTimeout time.Duration
        +ReconnectDelay time.Duration
    }
    
    class KVEvent {
        <<interface>>
        +GetType() EventType
        +GetTimestamp() time.Time
    }
    
    class BlockStoredEvent {
        +Type EventType
        +Timestamp time.Time
        +BlockHashes []int64
        +TokenIDs [][]int32
        +ParentBlockHash *int64
        +ModelName string
        +PodName string
    }
    
    class BlockRemovedEvent {
        +Type EventType
        +Timestamp time.Time
        +BlockHashes []int64
        +ModelName string
        +PodName string
    }
    
    class AllBlocksClearedEvent {
        +Type EventType
        +Timestamp time.Time
        +ModelName string
        +PodName string
    }
    
    class ZMQClientMetrics {
        -podKey string
        -connectionCount prometheus.Counter
        -disconnectionCount prometheus.Counter
        -eventsReceived *prometheus.CounterVec
        -eventProcessingTime prometheus.Observer
        -missedEvents prometheus.Counter
        -errors *prometheus.CounterVec
        
        +IncrementConnectionCount()
        +IncrementDisconnectionCount()
        +IncrementEventCount(eventType)
        +RecordEventProcessingLatency(duration)
        +IncrementMissedEvents(count)
        +IncrementErrorCount(errorType)
        +Delete()
    }
    
    Manager --> PodProvider : uses
    Manager --> SyncIndexProvider : uses
    Manager --> eventHandler : creates
    Manager --> ZMQClient : manages
    
    eventHandler ..|> EventHandler : implements
    eventHandler --> Manager : references
    
    ZMQClient --> ZMQClientConfig : uses
    ZMQClient --> EventHandler : calls
    ZMQClient --> ZMQClientMetrics : uses
    
    BlockStoredEvent ..|> KVEvent : implements
    BlockRemovedEvent ..|> KVEvent : implements
    AllBlocksClearedEvent ..|> KVEvent : implements
```

---

## 4. 组件交互时序图

```mermaid
sequenceDiagram
    participant K8s as K8s Informer
    participant Mgr as Manager
    participant ZMQ as ZMQClient
    participant vLLM as vLLM Pod
    participant Handler as eventHandler
    participant Sync as SyncIndexer

    %% Pod 添加流程
    rect rgb(230, 245, 255)
        Note over K8s,Sync: Pod 添加订阅流程
        K8s->>Mgr: OnPodAdd(pod)
        Mgr->>Mgr: isPodSubscribable(pod)
        Mgr->>Mgr: subscribeToPod(podKey, podInfo)
        Mgr->>ZMQ: NewZMQClient(config, handler)
        Mgr->>ZMQ: Start()
        ZMQ->>vLLM: Connect (SUB :5557)
        ZMQ->>vLLM: Connect (DEALER :5558)
        ZMQ->>vLLM: requestReplay(0)
        vLLM-->>ZMQ: Replay Response
        ZMQ->>ZMQ: consumeEventsWithReconnect()
    end
    
    %% 事件处理流程
    rect rgb(255, 245, 230)
        Note over K8s,Sync: KV Cache 事件处理流程
        loop Event Processing
            vLLM-->>ZMQ: [topic, seq, payload]
            ZMQ->>ZMQ: DecodeEventBatch(payload)
            ZMQ->>Handler: HandleEvent(event)
            alt BlockStoredEvent
                Handler->>Sync: ProcessBlockStored(syncEvent)
                Sync->>Sync: 更新 prefixMap
                Sync->>Sync: 更新 hashMapping
            else BlockRemovedEvent
                Handler->>Sync: ProcessBlockRemoved(syncEvent)
                Sync->>Sync: 清理 prefixMap
            end
            Handler-->>ZMQ: nil/error
            ZMQ->>ZMQ: 更新 lastSeq
        end
    end
    
    %% Pod 删除流程
    rect rgb(255, 235, 238)
        Note over K8s,Sync: Pod 删除清理流程
        K8s->>Mgr: OnPodDelete(pod)
        Mgr->>ZMQ: unsubscribeFromPod(podKey)
        ZMQ->>ZMQ: Stop()
        ZMQ->>vLLM: Close sockets
        Mgr->>Sync: RemovePrefix(modelName, loraID, podKey)
        Sync->>Sync: 清理该 Pod 的所有前缀
    end
```

---

## 5. ZMQ 连接状态机

```mermaid
stateDiagram-v2
    [*] --> Disconnected: 创建 ZMQClient
    
    Disconnected --> Connecting: Start()
    Connecting --> Connected: Connect() 成功
    Connecting --> Disconnected: Connect() 失败
    
    Connected --> Processing: consumeEvents()
    Processing --> Connected: 事件处理完成
    Processing --> Reconnecting: 处理错误
    
    Connected --> Reconnecting: 连接断开
    Reconnecting --> WaitingBackoff: handleReconnect()
    WaitingBackoff --> Connecting: 等待完成
    WaitingBackoff --> Stopped: ctx.Done()
    
    Connected --> ReplayRequested: 检测到序列号间隙
    ReplayRequested --> Connected: requestReplay() 成功
    ReplayRequested --> Reconnecting: requestReplay() 失败
    
    Connected --> Stopped: Stop()
    Processing --> Stopped: Stop()
    Reconnecting --> Stopped: Stop()
    
    Stopped --> [*]
    
    note right of Connected
        正常事件消费状态
        • Poll SUB socket
        • 处理消息
        • 更新序列号
    end note
    
    note right of Reconnecting
        重连状态
        • 指数退避
        • 最大 30s 间隔
    end note
    
    note right of ReplayRequested
        请求重放状态
        • 发送 DEALER 请求
        • 等待历史事件
    end note
```

---

## 6. 事件处理流程图

```mermaid
flowchart TD
    subgraph Input["输入"]
        MSG["ZMQ 消息<br/>[topic, seq, payload]"]
    end
    
    subgraph Decode["解码阶段"]
        RecvTopic["接收 topic"]
        RecvSeq["接收 seq (8 bytes)"]
        RecvPayload["接收 payload"]
        ParseSeq["解析序列号<br/>BigEndian.Uint64"]
        CheckSeq{"seq > lastSeq + 1?"}
        LogMissed["记录丢失事件数<br/>IncrementMissedEvents"]
        Unmarshal["MessagePack 解码"]
    end
    
    subgraph Process["处理阶段"]
        ParseType{"事件类型?"}
        
        subgraph BlockStored["BlockStored 处理"]
            BS_GetIndexer["获取 SyncIndexer"]
            BS_Convert["转换 Token<br/>[]int32 → []byte"]
            BS_Process["ProcessBlockStored()"]
        end
        
        subgraph BlockRemoved["BlockRemoved 处理"]
            BR_GetIndexer["获取 SyncIndexer"]
            BR_Process["ProcessBlockRemoved()"]
        end
        
        subgraph AllCleared["AllCleared 处理"]
            AC_Log["记录日志<br/>(未实现)"]
        end
    end
    
    subgraph Output["输出"]
        UpdateSeq["更新 lastSeq"]
        RecordMetrics["记录指标"]
        Done["完成"]
    end
    
    MSG --> RecvTopic
    RecvTopic --> RecvSeq
    RecvSeq --> RecvPayload
    RecvPayload --> ParseSeq
    ParseSeq --> CheckSeq
    CheckSeq -->|是| LogMissed
    CheckSeq -->|否| Unmarshal
    LogMissed --> Unmarshal
    
    Unmarshal --> ParseType
    ParseType -->|BLOCK_STORED| BS_GetIndexer
    ParseType -->|BLOCK_REMOVED| BR_GetIndexer
    ParseType -->|ALL_CLEARED| AC_Log
    
    BS_GetIndexer --> BS_Convert
    BS_Convert --> BS_Process
    BS_Process --> UpdateSeq
    
    BR_GetIndexer --> BR_Process
    BR_Process --> UpdateSeq
    
    AC_Log --> UpdateSeq
    
    UpdateSeq --> RecordMetrics
    RecordMetrics --> Done
    
    style MSG fill:#e3f2fd
    style Done fill:#e8f5e9
```

---

## 7. SyncPrefixHashTable 数据结构

```mermaid
flowchart TB
    subgraph HashTable["SyncPrefixHashTable"]
        direction TB
        
        subgraph Config["配置 (只读)"]
            Seed["seed: uint64"]
            MaxCtx["maxContexts: 1000"]
            MaxPfx["maxPrefixesPerContext: 10000"]
            BlockSize["blockSize: 16"]
        end
        
        subgraph ContextMap["contextMap (sync.Map)"]
            direction LR
            Ctx1["ModelContext 1<br/>{ModelName, LoraID}"]
            Ctx2["ModelContext 2<br/>{ModelName, LoraID}"]
            CtxN["ModelContext N"]
        end
        
        subgraph ContextData1["ContextData 1"]
            direction TB
            PS1["PrefixStore"]
            HM1["HashMapping"]
        end
        
        subgraph PrefixStore1["PrefixStore 结构"]
            direction TB
            PMap1["prefixMap<br/>map[uint64]map[string]*PodInfo"]
            CreateTime1["createTime"]
            LastAccess1["lastAccess (atomic)"]
            TotalPfx1["totalPrefixes"]
        end
        
        subgraph HashMapping1["HashMapping 结构"]
            E2A["engineToAibrix<br/>map[int64]uint64<br/>engine hash → aibrix hash"]
        end
        
        subgraph BlockIndex["blockIndex (全局)"]
            BI["map[int64][]ModelContext<br/>engine block → contexts"]
        end
    end
    
    Ctx1 --> ContextData1
    ContextData1 --> PS1
    ContextData1 --> HM1
    PS1 --> PrefixStore1
    HM1 --> HashMapping1
    
    style Seed fill:#fff3e0
    style PMap1 fill:#e3f2fd
    style E2A fill:#e8f5e9
```

---

## 8. 前缀匹配流程

```mermaid
flowchart TD
    subgraph Input["输入"]
        Tokens["tokens []byte<br/>用户输入 Token 序列"]
        Model["modelName, loraID"]
        ReadyPods["readyPods map"]
    end
    
    subgraph Hash["哈希计算"]
        GetCtx["获取 ModelContext"]
        ComputeHash["GetPrefixHashes(tokens)"]
        
        subgraph HashLoop["哈希循环"]
            direction TB
            Init["parentHash = seed"]
            Loop["for each block (blockSize=16)"]
            H1["hash = xxhash(parentHash + blockTokens)"]
            H2["prefixHashes.append(hash)"]
            H3["parentHash = hash"]
        end
    end
    
    subgraph Match["匹配"]
        LoadCtx["从 contextMap 加载"]
        CheckEvict{"markedForEviction?"}
        
        subgraph MatchLoop["顺序匹配循环"]
            direction TB
            M1["for i, prefixHash := range prefixHashes"]
            M2["pods = prefixMap[prefixHash]"]
            M3{"存在且非空?"}
            M4["计算匹配百分比<br/>(i+1)*100/len"]
            M5["检查 pod 是否 ready"]
            M6["prefixMatchPods[pod] = %"]
            M7{"有匹配?"}
            M8["break"]
        end
    end
    
    subgraph Output["输出"]
        Result["map[podName]int<br/>Pod → 匹配百分比"]
        AllHashes["[]uint64<br/>所有前缀哈希"]
    end
    
    Tokens --> ComputeHash
    Model --> GetCtx
    GetCtx --> LoadCtx
    ComputeHash --> HashLoop
    
    LoadCtx --> CheckEvict
    CheckEvict -->|是| Result
    CheckEvict -->|否| MatchLoop
    
    M1 --> M2
    M2 --> M3
    M3 -->|是| M4
    M3 -->|否| M8
    M4 --> M5
    M5 --> M6
    M6 --> M7
    M7 -->|否| M8
    M7 -->|是| M1
    
    M8 --> Result
    HashLoop --> AllHashes
    
    style Tokens fill:#e3f2fd
    style Result fill:#e8f5e9
```

---

## 9. 重连与重放机制

```mermaid
sequenceDiagram
    participant ZMQ as ZMQClient
    participant vLLM as vLLM Pod
    participant Handler as eventHandler
    
    Note over ZMQ,vLLM: 正常事件流
    vLLM-->>ZMQ: seq=10
    ZMQ->>Handler: HandleEvent
    
    Note over ZMQ,vLLM: 网络中断
    vLLM--xZMQ: seq=11 (丢失)
    vLLM--xZMQ: seq=12 (丢失)
    vLLM--xZMQ: seq=13 (丢失)
    
    Note over ZMQ: 检测到断连
    ZMQ->>ZMQ: markDisconnected()
    ZMQ->>ZMQ: metrics.IncrementDisconnectionCount()
    
    rect rgb(255, 245, 230)
        Note over ZMQ,vLLM: 指数退避重连
        loop Reconnect with Backoff
            ZMQ->>ZMQ: sleep(reconnectDelay)
            Note right of ZMQ: 1s → 2s → 4s → ... → 30s max
            ZMQ->>vLLM: Connect()
            alt 连接成功
                vLLM-->>ZMQ: Connected
                ZMQ->>ZMQ: reconnectDelay = 1s (重置)
            else 连接失败
                ZMQ->>ZMQ: reconnectDelay *= 2
            end
        end
    end
    
    rect rgb(230, 245, 255)
        Note over ZMQ,vLLM: 事件重放
        ZMQ->>vLLM: requestReplay(lastSeq+1=11)
        Note right of ZMQ: 通过 DEALER socket 发送
        vLLM-->>ZMQ: Replay Response
        Note right of vLLM: 包含 seq 11-13 的事件
        loop Process Replayed Events
            ZMQ->>Handler: HandleEvent(seq=11)
            ZMQ->>Handler: HandleEvent(seq=12)
            ZMQ->>Handler: HandleEvent(seq=13)
        end
    end
    
    Note over ZMQ,vLLM: 恢复正常事件流
    vLLM-->>ZMQ: seq=14
    ZMQ->>Handler: HandleEvent
```

---

## 10. 指标监控架构

```mermaid
flowchart TB
    subgraph ZMQClient["ZMQ Client 指标"]
        direction TB
        
        subgraph ConnMetrics["连接指标"]
            C1["kvcache_zmq_connections_total<br/>连接建立次数"]
            C2["kvcache_zmq_disconnections_total<br/>断连次数"]
            C3["kvcache_zmq_reconnect_attempts_total<br/>重连尝试次数"]
            C4["kvcache_zmq_connection_status<br/>当前状态 (0/1)"]
        end
        
        subgraph EventMetrics["事件指标"]
            E1["kvcache_zmq_events_received_total<br/>接收事件总数"]
            E2["kvcache_zmq_events_processed_total<br/>处理事件总数"]
            E3["kvcache_zmq_event_processing_duration_seconds<br/>处理延迟分布"]
            E4["kvcache_zmq_missed_events_total<br/>丢失事件数"]
        end
        
        subgraph ReplayMetrics["重放指标"]
            R1["kvcache_zmq_replay_requests_total<br/>重放请求数"]
            R2["kvcache_zmq_replay_success_total<br/>重放成功数"]
            R3["kvcache_zmq_replay_failures_total<br/>重放失败数"]
        end
        
        subgraph ErrorMetrics["错误指标"]
            Er1["kvcache_zmq_errors_total<br/>错误计数 (按类型)"]
        end
        
        subgraph StateMetrics["状态指标"]
            S1["kvcache_zmq_last_sequence_id<br/>最后序列号"]
        end
    end
    
    subgraph Labels["标签维度"]
        L1["pod_key: namespace/name"]
        L2["event_type: BLOCK_STORED/REMOVED/CLEARED"]
        L3["error_type: consume_events/reconnect/decode/handle_event"]
    end
    
    subgraph Prometheus["Prometheus"]
        Scrape["Scrape /metrics"]
        Store["时序数据存储"]
    end
    
    subgraph Grafana["Grafana Dashboard"]
        D1["连接状态面板"]
        D2["事件吞吐量面板"]
        D3["延迟分布面板"]
        D4["错误率面板"]
    end
    
    ZMQClient -->|"暴露"| Scrape
    Scrape --> Store
    Store --> Grafana
    
    style ConnMetrics fill:#e3f2fd
    style EventMetrics fill:#e8f5e9
    style ReplayMetrics fill:#fff3e0
    style ErrorMetrics fill:#ffebee
```

---

## 总结图示

### 系统角色定位

```mermaid
flowchart LR
    subgraph 推理层["推理层 (vLLM)"]
        Engine["推理引擎"]
        KVC["KV Cache"]
    end
    
    subgraph 网关层["网关层 (Gateway)"]
        KVEM["KV Event Manager<br/>━━━━━━━━━━━━<br/>• 事件订阅<br/>• 状态同步"]
        Sync["Sync Indexer<br/>━━━━━━━━━━━━<br/>• 前缀存储<br/>• 匹配查询"]
        Router["Prefix Router<br/>━━━━━━━━━━━━<br/>• 路由决策<br/>• 负载均衡"]
    end
    
    subgraph 客户端["客户端"]
        User["用户请求"]
    end
    
    Engine --> KVC
    KVC -->|"KV Events"| KVEM
    KVEM -->|"同步"| Sync
    User -->|"推理请求"| Router
    Router -->|"查询前缀"| Sync
    Router -->|"选择 Pod"| Engine
    
    style KVEM fill:#e1f5fe,stroke:#0288d1
    style Sync fill:#fff3e0,stroke:#ff9800
    style Router fill:#e8f5e9,stroke:#4caf50
```

