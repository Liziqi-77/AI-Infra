flowchart TB
    Client[Client]
    
    subgraph Proxy["conductor-proxy (C++/Python)"]
        RC[Request Classification]
        SD[Scheduling Decision<br/>- Mixed Node Selection<br/>- PD Separation Logic]
        RF[Request Forwarding<br/>- Stream/Non-stream<br/>- Connection Pooling]
        
        RC --> SD
        SD --> RF
    end
    
    subgraph Nodes["Backend Nodes"]
        PrefillNode[Prefill Node]
        DecodeNode[Decode Node]
        MixedNode[Mixed Node]
    end
    
    subgraph Ctrl["kv event manager (Go)"]
        ZMQSub[ZMQ Event Subscriber]
        EventHandler[Event Handler<br/>- BlockStoredEvent<br/>- BlockRemovedEvent UpdateEvent<br/>-]
        PrefixIndexer[Prefix Cache Indexer<br/>- Prefix Hash Table<br/>- Cache Hit Computation]
        
        ZMQSub --> EventHandler
        EventHandler --> PrefixIndexer
    end
    
    Client -->|HTTP Request| RC
    RF -->|HTTP| PrefillNode
    RF -->|HTTP| DecodeNode
    RF -->|HTTP| MixedNode
    
    PrefillNode -->|ZMQ Events| ZMQSub
    DecodeNode -->|ZMQ Events| ZMQSub
    MixedNode -->|ZMQ Events| ZMQSub
    
    style Proxy fill:#e1f5ff
    style Ctrl fill:#fff4e1
    style Nodes fill:#e8f5e9




