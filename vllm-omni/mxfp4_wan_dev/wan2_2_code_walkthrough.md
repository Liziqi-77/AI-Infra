# Wan2.2 模型完整调用链文档

> 拉起命令：`vllm serve ./wan_2.2 --omni --port 8001`

---

## 1. 总体架构概览

```
用户 HTTP 请求
    │
    ▼
┌─────────────────────────────────────────────────────────────────┐
│                     FastAPI API Server                          │
│  (vllm_omni/entrypoints/openai/api_server.py)                  │
│  - /v1/videos 端点 → OmniOpenAIServingVideo                     │
│  - /v1/chat/completions 端点 → OmniOpenAIServingChat             │
└─────────────────────────────────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────────────────────────────────┐
│                       AsyncOmni                                 │
│  (vllm_omni/entrypoints/async_omni.py)                          │
│  - 统一编排器，管理多阶段 pipeline                                │
│  - 对外暴露 generate() 接口                                      │
└─────────────────────────────────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────────────────────────────────┐
│                    AsyncOmniEngine                              │
│  (vllm_omni/engine/async_omni_engine.py)                        │
│  - 在后台线程中运行 Orchestrator                                  │
│  - 通过 janus 队列与 AsyncOmni 通信                               │
└─────────────────────────────────────────────────────────────────┘
    │
    ├── Stage 0 (如果有 LLM 阶段) → StageEngineCoreClient (vLLM)
    │
    └── Stage N (Diffusion 阶段)
            │
            ▼
        ┌─────────────────────────────────────────────────────────┐
        │              StageDiffusionClient                        │
        │  (vllm_omni/diffusion/stage_diffusion_client.py)        │
        │  - 通过 ZMQ 与子进程通信                                  │
        └─────────────────────────────────────────────────────────┘
            │ ZMQ (PUSH/PULL)
            ▼
        ┌─────────────────────────────────────────────────────────┐
        │              StageDiffusionProc (子进程)                  │
        │  (vllm_omni/diffusion/stage_diffusion_proc.py)          │
        │  - 运行 DiffusionEngine                                  │
        └─────────────────────────────────────────────────────────┘
            │
            ▼
        ┌─────────────────────────────────────────────────────────┐
        │                   DiffusionEngine                        │
        │  (vllm_omni/diffusion/diffusion_engine.py)              │
        │  - 请求调度 + 预处理/后处理                                │
        └─────────────────────────────────────────────────────────┘
            │
            ▼
        ┌─────────────────────────────────────────────────────────┐
        │              MultiprocDiffusionExecutor                  │
        │  (vllm_omni/diffusion/executor/multiproc_executor.py)   │
        │  - 多 GPU 工作进程管理                                     │
        └─────────────────────────────────────────────────────────┘
            │ MessageQueue (共享内存广播)
            ▼
        ┌─────────────────────────────────────────────────────────┐
        │                  WorkerProc (GPU 进程)                    │
        │  (vllm_omni/diffusion/worker/diffusion_worker.py)       │
        │  - DiffusionWorker → DiffusionModelRunner                │
        │  - 加载模型 + 执行前向传播                                 │
        └─────────────────────────────────────────────────────────┘
            │
            ▼
        ┌─────────────────────────────────────────────────────────┐
        │              DiffusersPipelineLoader                     │
        │  (vllm_omni/diffusion/model_loader/diffusers_loader.py) │
        │  - 加载 Pipeline 组件 (transformer, VAE, text_encoder)   │
        │  - 加载权重 + 量化后处理                                   │
        └─────────────────────────────────────────────────────────┘
            │
            ▼
        ┌─────────────────────────────────────────────────────────┐
        │                   Wan22Pipeline                          │
        │  (vllm_omni/diffusion/models/wan2_2/pipeline_wan2_2.py) │
        │  - forward(): 去噪循环 → transformer → VAE 解码           │
        └─────────────────────────────────────────────────────────┘
            │
            ▼
        ┌─────────────────────────────────────────────────────────┐
        │              WanTransformer3DModel                       │
        │  (vllm_omni/diffusion/models/wan2_2/wan2_2_transformer) │
        │  - Patch Embedding → Transformer Blocks → Unpatchify     │
        └─────────────────────────────────────────────────────────┘
```

---

## 2. 从 CLI 命令到服务启动的完整调用链

### 2.1 CLI 入口解析

**命令**：`vllm serve ./wan_2.2 --omni --port 8001`

**入口文件**：`vllm_omni/entrypoints/cli/main.py`

```
main()
  │
  ├── 检测 sys.argv 中是否有 "--omni" 标志
  │   ├── 没有 → 转发到 vLLM 原生 main()
  │   └── 有 → 进入 vLLM-Omni 路径
  │
  ├── cli_env_setup()                          # 设置 vLLM 环境变量
  ├── _ensure_vllm_platform()                  # 确保 vLLM 平台检测有效
  │     └── 如果 vLLM 平台未指定，用 omni 平台或 CpuPlatform 替代
  │
  ├── 注册子命令模块：
  │     ├── vllm_omni.entrypoints.cli.serve    # serve 命令
  │     └── vllm_omni.entrypoints.cli.benchmark.main
  │
  ├── parser.parse_args()                      # 解析命令行参数
  │     └── make_arg_parser()                  # vLLM 的参数解析器，添加所有 serve 选项
  │         └── 添加 OmniConfig 参数组：
  │             ├── --omni, --port, --model
  │             ├── --boundary-ratio           # Wan2.2 双 transformer 切换比例
  │             ├── --flow-shift               # 调度器 flow shift
  │             ├── --cfg-parallel-size        # CFG 并行大小
  │             ├── --ulysses-degree / --ring-degree  # 序列并行
  │             ├── --quantization-config      # 量化配置
  │             ├── --use-hsdp                 # HSDP 权重分片
  │             ├── --cache-backend            # 缓存后端
  │             └── ...
  │
  └── args.dispatch_function(args)             # 调用 OmniServeCommand.cmd()
```

### 2.2 OmniServeCommand.cmd()

**文件**：`vllm_omni/entrypoints/cli/serve.py`

```python
def cmd(args):
    log_logo()                                  # 打印 Logo
    args.model = args.model_tag                 # 模型路径: "./wan_2.2"
    uvloop.run(omni_run_server(args))           # 启动异步服务器
```

### 2.3 omni_run_server()

**文件**：`vllm_omni/entrypoints/openai/api_server.py`

```
omni_run_server(args)
  │
  ├── setup_openai_server(args)                 # 设置服务器地址/端口
  │
  └── omni_run_server_worker(listen_address, sock, args)
        │
        ├── build_async_omni(args)              # 创建 AsyncOmni 实例（异步上下文管理器）
        │     │
        │     └── build_async_omni_from_stage_config(args)
        │           │
        │           └── AsyncOmni(model=args.model, **kwargs)
        │                 │
        │                 └── OmniBase.__init__(model=model, **kwargs)
        │                       │
        │                       ├── omni_snapshot_download(model)  # 下载/缓存模型
        │                       │
        │                       └── AsyncOmniEngine(             # 核心引擎初始化
        │                             model=model,
        │                             init_timeout=600,
        │                             stage_init_timeout=300,
        │                             **kwargs
        │                           )
        │
        ├── app = build_openai_app(args, supported_tasks)  # 构建 FastAPI 应用
        ├── _remove_route_from_app(app, "/v1/chat/completions")  # 移除上游路由
        ├── omni_init_app_state(engine_client, app.state, args)  # 初始化应用状态
        │     │
        │     ├── 检测是否为 pure diffusion 模式（单 diffusion stage）
        │     │     └── is_pure_diffusion = True（Wan2.2 是纯 diffusion 模型）
        │     │
        │     ├── state.diffusion_engine = engine_client
        │     ├── state.openai_serving_video = OmniOpenAIServingVideo.for_diffusion(...)
        │     └── state.openai_serving_chat = OmniOpenAIServingChat.for_diffusion(...)
        │
        └── serve_http(app, ...)                # 启动 Uvicorn HTTP 服务器
```

### 2.4 AsyncOmniEngine 初始化

**文件**：`vllm_omni/engine/async_omni_engine.py`

```
AsyncOmniEngine.__init__(model, ...)
  │
  ├── self._resolve_stage_configs(model, kwargs)  # 解析 stage 配置
  │     └── load_and_resolve_stage_configs()      # 从模型或 YAML 加载
  │         └── 对于 diffusion 模型：创建默认 diffusion stage config
  │
  ├── 启动 Orchestrator 后台线程
  │     └── threading.Thread(target=self._bootstrap_orchestrator, ...)
  │
  └── startup_future.result(timeout=600)        # 等待初始化完成
```

### 2.5 _bootstrap_orchestrator() — 核心初始化流程

```
_bootstrap_orchestrator(stage_init_timeout, startup_future)
  │
  ├── asyncio.new_event_loop()                  # 创建事件循环
  │
  └── _run_orchestrator()
        │
        ├── self._initialize_janus_queues()     # 创建 janus 队列（线程安全队列）
        │     ├── request_queue                 # 请求队列
        │     ├── output_queue                  # 输出队列
        │     └── rpc_output_queue              # RPC 输出队列
        │
        ├── self._initialize_stages(stage_init_timeout)  # 初始化所有 stage
        │     │
        │     ├── prepare_engine_environment()   # 准备引擎环境
        │     ├── load_omni_transfer_config_for_model()  # 加载传输配置
        │     │
        │     └── for stage_id, stage_cfg in enumerate(stage_configs):
        │           │
        │           ├── 提取 stage metadata
        │           │
        │           ├── 如果是 diffusion stage:
        │           │     │
        │           │     └── initialize_diffusion_stage(model, stage_cfg, metadata)
        │           │           │
        │           │           ├── OmniDiffusionConfig.from_kwargs(model, **engine_args)
        │           │           │     │
        │           │           │     ├── 解析并行配置 (tensor_parallel, ulysses, ring, cfg_parallel...)
        │           │           │     ├── 解析量化配置 (quantization_config)
        │           │           │     │     ├── 如果是 str → build_quant_config(str)
        │           │           │     │     ├── 如果是 dict → build_quant_config(dict)
        │           │           │     │     └── 如果是 QuantizationConfig → 直接使用
        │           │           │     ├── 从模型 config 自动检测量化 (tf_model_config.quant_config)
        │           │           │     └── 设置 boundary_ratio, flow_shift 等 Wan2.2 特有参数
        │           │           │
        │           │           └── StageDiffusionClient(model, od_config, metadata)
        │           │                 │
        │           │                 └── spawn_diffusion_proc(model, od_config)
        │           │                       │
        │           │                       └── 启动 StageDiffusionProc 子进程
        │           │                             │
        │           │                             └── StageDiffusionProc.run_diffusion_proc()
        │           │                                   │
        │           │                                   ├── proc = StageDiffusionProc(model, od_config)
        │           │                                   ├── proc.initialize()
        │           │                                   │     │
        │           │                                   │     ├── _enrich_config()
        │           │                                   │     │     │
        │           │                                   │     │     ├── 从 HF 加载 model_index.json
        │           │                                   │     │     │     └── od_config.model_class_name = "WanPipeline"
        │           │                                   │     │     │
        │           │                                   │     │     └── 加载 transformer/config.json
        │           │                                   │     │           └── od_config.tf_model_config = TransformerConfig(...)
        │           │                                   │     │
        │           │                                   │     └── DiffusionEngine.make_engine(od_config)
        │           │                                   │           │
        │           │                                   │           ├── 获取 post_process_func / pre_process_func
        │           │                                   │           │     └── 从 registry 根据 model_class_name 查找
        │           │                                   │           │
        │           │                                   │           ├── executor_class = DiffusionExecutor.get_class(od_config)
        │           │                                   │           │     └── 默认返回 MultiprocDiffusionExecutor
        │           │                                   │           │
        │           │                                   │           ├── self.executor = executor_class(od_config)
        │           │                                   │           │     │
        │           │                                   │           │     └── MultiprocDiffusionExecutor._init_executor()
        │           │                                   │           │           │
        │           │                                   │           │           └── _launch_workers(broadcast_handle)
        │           │                                   │           │                 │
        │           │                                   │           │                 └── for i in range(num_gpus):
        │           │                                   │           │                       │
        │           │                                   │           │                       └── mp.Process(
        │           │                                   │           │                             target=WorkerProc.worker_main,
        │           │                                   │           │                             args=(i, od_config, ...)
        │           │                                   │           │                           )
        │           │                                   │           │
        │           │                                   │           └── self._dummy_run()   # 预热运行
        │           │                                   │
        │           │                                   └── 发送 READY 信号（handshake）
        │           │
        │           └── complete_diffusion_handshake(proc, handshake_address)
        │                 └── 等待子进程发送 READY 信号
        │
        ├── orchestrator = Orchestrator(...)    # 创建编排器
        │     │
        │     ├── request_async_queue            # 接收请求
        │     ├── output_async_queue             # 发送输出
        │     ├── stage_clients                  # 各 stage 客户端
        │     └── output_processors              # 输出处理器
        │
        ├── startup_future.set_result(...)      # 通知主线程初始化完成
        │
        └── await orchestrator.run()            # 运行编排器主循环
```

### 2.6 GPU Worker 进程初始化

**文件**：`vllm_omni/diffusion/worker/diffusion_worker.py`

```
WorkerProc.worker_main(rank, od_config, pipe_writer, broadcast_handle, ...)
  │
  ├── load_omni_general_plugins()               # 加载插件
  │
  ├── worker_proc = WorkerProc(od_config, gpu_id=rank, ...)
  │     │
  │     ├── zmq.Context()                       # 创建 ZMQ 上下文
  │     ├── MessageQueue.create_from_handle()   # 创建广播消息队列读取器
  │     │
  │     └── self.worker = self._create_worker(gpu_id, od_config, ...)
  │           │
  │           └── WorkerWrapperBase(gpu_id, od_config, ...)
  │                 │
  │                 └── worker_class(local_rank, rank, od_config, skip_load_model=...)
  │                       │
  │                       └── DiffusionWorker.__init__(local_rank, rank, od_config)
  │                             │
  │                             ├── self.init_device()
  │                             │     │
  │                             │     ├── 设置 MASTER_ADDR/MASTER_PORT/LOCAL_RANK/RANK/WORLD_SIZE
  │                             │     ├── current_omni_platform.get_torch_device(rank)
  │                             │     ├── current_omni_platform.set_device(device)
  │                             │     │
  │                             │     ├── 创建 VllmConfig（并行配置）
  │                             │     │
  │                             │     ├── init_distributed_environment(world_size, rank)  # NCCL 初始化
  │                             │     │
  │                             │     └── initialize_model_parallel(
  │                             │           data_parallel_size, cfg_parallel_size,
  │                             │           sequence_parallel_size, ulysses_degree, ring_degree,
  │                             │           tensor_parallel_size, pipeline_parallel_size,
  │                             │           fully_shard_degree, hsdp_replicate_size, ...
  │                             │         )
  │                             │
  │                             ├── self.model_runner = DiffusionModelRunner(vllm_config, od_config, device)
  │                             │
  │                             └── self.load_model(load_format=od_config.diffusion_load_format)
  │                                   │
  │                                   └── DiffusionModelRunner.load_model(...)
  │                                         │
  │                                         ├── load_device = "cpu" if cpu_offload else device
  │                                         │
  │                                         ├── model_loader = DiffusersPipelineLoader(load_config, od_config)
  │                                         │
  │                                         └── self.pipeline = model_loader.load_model(
  │                                               od_config, load_device, load_format, device
  │                                             )
  │                                               │
  │                                               ├── initialize_model(od_config)   # 从 registry 创建 Pipeline
  │                                               │     │
  │                                               │     ├── DiffusionModelRegistry._try_load_model_cls("WanPipeline")
  │                                               │     │     └── 查找 _DIFFUSION_MODELS["WanPipeline"]
  │                                               │     │           = ("wan2_2", "pipeline_wan2_2", "Wan22Pipeline")
  │                                               │     │
  │                                               │     ├── model = Wan22Pipeline(od_config=od_config)
  │                                               │     │     │
  │                                               │     │     ├── 读取 model_index.json
  │                                               │     │     │     ├── expand_timesteps = False
  │                                               │     │     │     └── has_transformer_2 = (是否存在 transformer_2 目录)
  │                                               │     │     │
  │                                               │     │     ├── 根据 boundary_ratio 决定加载哪些 transformer
  │                                               │     │     │     ├── load_transformer = boundary_ratio != 1.0
  │                                               │     │     │     └── load_transformer_2 = has_transformer_2 and boundary_ratio != 0.0
  │                                               │     │     │
  │                                               │     │     ├── 设置 weights_sources（权重来源）
  │                                               │     │     │     ├── ComponentSource(model, subfolder="transformer", prefix="transformer.")
  │                                               │     │     │     └── ComponentSource(model, subfolder="transformer_2", prefix="transformer_2.")
  │                                               │     │     │
  │                                               │     │     ├── self.tokenizer = AutoTokenizer.from_pretrained(model, subfolder="tokenizer")
  │                                               │     │     ├── self.text_encoder = UMT5EncoderModel.from_pretrained(model, subfolder="text_encoder")
  │                                               │     │     ├── self.vae = DistributedAutoencoderKLWan.from_pretrained(model, subfolder="vae")
  │                                               │     │     │
  │                                               │     │     ├── 创建 transformer（仅初始化结构，不加载权重）
  │                                               │     │     │     ├── load_transformer_config(model, "transformer")  # 读取 config.json
  │                                               │     │     │     ├── create_transformer_from_config(config)
  │                                               │     │     │     │     └── WanTransformer3DModel(**kwargs)
  │                                               │     │     │     │           │
  │                                               │     │     │     │           ├── self.rope = WanRotaryPosEmbed(...)
  │                                               │     │     │     │           ├── self.patch_embedding = Conv3dLayer(...)
  │                                               │     │     │     │           ├── self.condition_embedder = WanTimeTextImageEmbedding(...)
  │                                               │     │     │     │           ├── self.blocks = nn.ModuleList([WanTransformerBlock(...) for _ in range(num_layers)])
  │                                               │     │     │     │           ├── self.norm_out = FP32LayerNorm(...)
  │                                               │     │     │     │           └── self.proj_out = nn.Linear(...)
  │                                               │     │     │     │
  │                                               │     │     │     └── 同理创建 transformer_2（如果是 MoE 模型）
  │                                               │     │     │
  │                                               │     │     └── self.scheduler = FlowUniPCMultistepScheduler(...)
  │                                               │     │
  │                                               │     ├── 配置 VAE 优化（slicing/tiling）
  │                                               │     └── _apply_sequence_parallel_if_enabled(model, od_config)
  │                                               │           └── 如果 SP 启用，对 transformer 应用 _sp_plan hooks
  │                                               │
  │                                               ├── self.load_weights(model)          # 加载权重
  │                                               │     │
  │                                               │     ├── get_all_weights(model)      # 获取权重迭代器
  │                                               │     │     │
  │                                               │     │     └── for source in weights_sources:
  │                                               │     │           └── _get_weights_iterator(source)
  │                                               │     │                 │
  │                                               │     │                 ├── _prepare_weights()  # 准备权重文件
  │                                               │     │                 │     ├── 检查本地/下载 safetensors 文件
  │                                               │     │                 │     └── filter_duplicate_safetensors_files()
  │                                               │     │                 │
  │                                               │     │                 └── safetensors_weights_iterator()  # 或多线程版本
  │                                               │     │                       └── 逐个加载 safetensors 文件中的张量
  │                                               │     │
  │                                               │     └── model.load_weights(weights_iterator)
  │                                               │           │
  │                                               │           └── AutoWeightsLoader(self).load_weights(weights)
  │                                               │                 │
  │                                               │                 └── 遍历权重，调用各模块的 load_weights()
  │                                               │                       │
  │                                               │                       └── WanTransformer3DModel.load_weights(weights)
  │                                               │                             │
  │                                               │                             ├── QKV 融合映射：
  │                                               │                             │     .attn1.to_q + .attn1.to_k + .attn1.to_v → .attn1.to_qkv
  │                                               │                             │
  │                                               │                             ├── 名称重映射：
  │                                               │                             │     "scale_shift_table" → "output_scale_shift_prepare.scale_shift_table"
  │                                               │                             │     ".ffn.net.0." → ".ffn.net_0."
  │                                               │                             │     ".to_out.0." → ".to_out."
  │                                               │                             │
  │                                               │                             ├── TP 分片的 RMSNorm 权重处理：
  │                                               │                             │     └── 按 rank 切分 norm_q/norm_k 权重
  │                                               │                             │
  │                                               │                             └── weight_loader(param, loaded_weight)
  │                                               │
  │                                               └── _process_weights_after_loading(model, device)  # 量化后处理
  │                                                     │
  │                                                     └── for module in model.named_modules():
  │                                                           └── if module.quant_method is not None:
  │                                                                 └── quant_method.process_weights_after_loading(module)
  │                                                                       # FP8 量化：计算 weight_scale，转换权重为 FP8
  │
  │                                         ├── 应用 offloading（如果启用）
  │                                         │     └── get_offload_backend(od_config).enable(pipeline)
  │                                         │
  │                                         ├── 应用 torch.compile（如果不使用 eager mode）
  │                                         │     └── regionally_compile(transformer, dynamic=True)
  │                                         │
  │                                         └── 设置 cache backend（如果启用）
  │                                               └── get_cache_backend(backend, config).enable(pipeline)
  │
  │                             └── self.init_lora_manager()          # 初始化 LoRA 管理器
  │
  ├── pipe_writer.send({"status": "ready", "result_handle": ...})  # 发送就绪信号
  │
  └── worker_proc.worker_busy_loop()            # 进入主循环，等待 RPC 请求
```

---

## 3. 从 HTTP 请求到模型推理的完整调用链

### 3.1 HTTP 请求处理

**用户请求**：`POST /v1/videos`

```
HTTP POST /v1/videos
  │
  ▼
OmniOpenAIServingVideo.generate_videos(request, reference_id)
  │
  ├── _run_and_extract(request, reference_id)
  │     │
  │     ├── 构建 OmniTextPrompt
  │     ├── 构建 OmniDiffusionSamplingParams
  │     │     ├── width, height, num_frames, fps
  │     │     ├── num_inference_steps, guidance_scale, guidance_scale_2
  │     │     ├── seed, boundary_ratio, flow_shift
  │     │     └── extra_args（额外参数）
  │     │
  │     └── _run_generation(prompt, gen_params, reference_id)
  │           │
  │           └── self._engine_client.generate(
  │                 prompt=prompt,
  │                 sampling_params_list=[gen_params],
  │                 request_id=reference_id
  │               )
  │
  └── 编码视频为 base64 → VideoGenerationResponse
```

### 3.2 AsyncOmni.generate()

**文件**：`vllm_omni/entrypoints/async_omni.py`

```
AsyncOmni.generate(prompt, sampling_params_list, request_id)
  │
  ├── self._final_output_handler()            # 启动最终输出分发器（首次调用时）
  │     │
  │     └── asyncio.create_task(_final_output_loop())
  │           │
  │           └── while True:
  │                 msg = await engine.try_get_output_async()  # 从 Orchestrator 获取输出
  │                 └── 路由到对应 request 的 queue
  │
  ├── self.resolve_sampling_params_list(sampling_params_list)  # 解析采样参数
  │
  ├── self.engine.add_request_async(          # 添加请求到 stage 0
  │       request_id, prompt, sampling_params_list, final_stage_id
  │     )
  │     │
  │     └── 构建 add_request message → 放入 request_queue
  │
  └── self._process_orchestrator_results(request_id, ...)
        │
        └── while True:
              result = await req_state.queue.get()  # 等待结果
              │
              ├── 检查错误
              ├── 构建 OmniRequestOutput
              └── yield output
```

### 3.3 Orchestrator 处理请求

**文件**：`vllm_omni/engine/orchestrator.py`

```
Orchestrator.run()
  │
  └── while True:
        msg = await request_async_queue.get()   # 从请求队列获取
        │
        ├── 如果是 add_request:
        │     │
        │     └── 转发到 stage 0 的 add_request_async()
        │           │
        │           ├── 如果是 Diffusion stage:
        │           │     └── StageDiffusionClient.add_request_async()
        │           │           │
        │           │           └── ZMQ PUSH → StageDiffusionProc
        │           │
        │           └── 如果是 LLM stage:
        │                 └── StageEngineCoreClient.add_request_async()
        │
        └── 从 stage 获取输出 → 放入 output_async_queue
```

### 3.4 StageDiffusionProc 处理请求

**文件**：`vllm_omni/diffusion/stage_diffusion_proc.py`

```
StageDiffusionProc.run_loop()
  │
  └── while True:
        raw = await request_socket.recv()       # ZMQ PULL 接收
        msg = decoder.decode(raw)
        │
        ├── if msg_type == "add_request":
        │     └── asyncio.create_task(_dispatch_request(...))
        │           │
        │           └── self._process_request(request_id, prompt, sampling_params_dict)
        │                 │
        │                 ├── 构建 OmniDiffusionRequest
        │                 │
        │                 └── loop.run_in_executor(self._executor, self._engine.step, request)
        │                       │
        │                       └── DiffusionEngine.step(request)
        │                             │
        │                             ├── 预处理：self.pre_process_func(request)
        │                             │
        │                             ├── self.add_req_and_wait_for_response(request)
        │                             │     │
        │                             │     └── self.executor.add_req(request)
        │                             │           │
        │                             │           └── MultiprocDiffusionExecutor.add_req(request)
        │                             │                 │
        │                             │                 ├── 广播 RPC 到所有 worker
        │                             │                 │     └── broadcast_mq.enqueue({"type": "rpc", "method": "generate", ...})
        │                             │                 │
        │                             │                 └── 等待 result_mq 响应
        │                             │
        │                             ├── 后处理：self.post_process_func(output_data)
        │                             │
        │                             └── 返回 OmniRequestOutput
        │
        └── 发送响应：response_socket.send(encoder.encode({"type": "result", "output": result}))
```

### 3.5 Worker 执行模型推理

**文件**：`vllm_omni/diffusion/worker/diffusion_worker.py`

```
WorkerProc.worker_busy_loop()
  │
  └── while self._running:
        msg = self.recv_message()               # 从广播队列接收
        │
        ├── if msg_type == "rpc":
        │     └── self.execute_rpc(msg)
        │           │
        │           └── self.worker.execute_method(method, *args, **kwargs)
        │                 │
        │                 ├── if method == "generate":
        │                 │     └── self.worker.generate(requests)
        │                 │           │
        │                 │           └── self.execute_model(requests, od_config)
        │                 │                 │
        │                 │                 └── self.model_runner.execute_model(req)
        │                 │                       │
        │                 │                       ├── self.kv_transfer_manager.receive_multi_kv_cache_distributed(req)
        │                 │                       │
        │                 │                       ├── 设置 generator（seed）
        │                 │                       │
        │                 │                       ├── 刷新 cache backend（如果启用）
        │                 │                       │
        │                 │                       ├── self.pipeline.forward(req)  # 核心前向传播
        │                 │                       │     │
        │                 │                       │     └── Wan22Pipeline.forward(req, ...)
        │                 │                       │           │
        │                 │                       │           ├── encode_prompt()          # 文本编码
        │                 │                       │           │     └── self.text_encoder(tokenized_prompt)
        │                 │                       │           │
        │                 │                       │           ├── prepare_latents()        # 准备初始噪声
        │                 │                       │           │     └── randn_tensor(shape, generator=generator)
        │                 │                       │           │
        │                 │                       │           ├── 去噪循环：for t in timesteps:
        │                 │                       │           │     │
        │                 │                       │           │     ├── 根据 boundary_timestep 选择 transformer/transformer_2
        │                 │                       │           │     │
        │                 │                       │           │     ├── predict_noise_maybe_with_cfg()
        │                 │                       │           │     │     │
        │                 │                       │           │     │     ├── positive_kwargs: forward(transformer, prompt_embeds)
        │                 │                       │           │     │     │     │
        │                 │                       │           │     │     │     └── current_model.forward(
        │                 │                       │           │     │     │           hidden_states, timestep, encoder_hidden_states
        │                 │                       │           │     │     │         )
        │                 │                       │           │     │     │           │
        │                 │                       │           │     │     │           ├── WanTransformer3DModel.forward()
        │                 │                       │           │     │     │           │     │
        │                 │                       │           │     │     │           │     ├── rope(hidden_states)          # 旋转位置编码
        │                 │                       │           │     │     │           │     ├── patch_embedding(hidden_states)  # 3D 卷积
        │                 │                       │           │     │     │           │     ├── condition_embedder(timestep, text)  # 条件嵌入
        │                 │                       │           │     │     │           │     │
        │                 │                       │           │     │     │           │     └── for block in self.blocks:
        │                 │                       │           │     │     │           │           │
        │                 │                       │           │     │     │           │           └── WanTransformerBlock.forward()
        │                 │                       │           │     │     │           │                 │
        │                 │                       │           │     │     │           │                 ├── attn1 (自注意力)
        │                 │                       │           │     │     │           │                 │     ├── to_qkv(hidden_states)  # QKV 并行线性
        │                 │                       │           │     │     │           │                 │     ├── norm_q(query), norm_k(key)  # DistributedRMSNorm
        │                 │                       │           │     │     │           │                 │     ├── apply_rotary_emb_wan(query/key, freqs_cos/sin)
        │                 │                       │           │     │     │           │                 │     └── self.attn(query, key, value)  # vLLM Attention
        │                 │                       │           │     │     │           │                 │
        │                 │                       │           │     │     │           │                 ├── attn2 (交叉注意力)
        │                 │                       │           │     │     │           │                 │     ├── to_q(hidden_states)
        │                 │                       │           │     │     │           │                 │     ├── to_k(encoder_hidden_states), to_v(encoder_hidden_states)
        │                 │                       │           │     │     │           │                 │     └── self.attn(query, key, value)
        │                 │                       │           │     │     │           │                 │
        │                 │                       │           │     │     │           │                 └── ffn(hidden_states)
        │                 │                       │           │     │     │           │                       ├── net_0 (ColumnParallelGELU)
        │                 │                       │           │     │     │           │                       └── net_2 (RowParallelLinear)
        │                 │                       │           │     │     │           │
        │                 │                       │           │     │     │           ├── norm_out + scale/shift
        │                 │                       │           │     │     │           ├── proj_out
        │                 │                       │           │     │     │           └── unpatchify → output tensor
        │                 │                       │           │     │     │
        │                 │                       │           │     │     └── 如果 CFG: 同样计算 negative prompt
        │                 │                       │           │     │
        │                 │                       │           │     └── noise_pred = cfg_scale * (pos - neg) + neg
        │                 │                       │           │
        │                 │                       │           └── scheduler_step(noise_pred, t, latents)  # 调度器步进
        │                 │                       │
        │                 │                       │           └── vae.decode(latents)  # VAE 解码
        │                 │                       │
        │                 │                       └── self._record_peak_memory(output)
        │                 │
        │                 └── 返回 DiffusionOutput
        │
        └── self.return_result(output)          # 通过 result_mq 返回结果
```

---

## 4. 量化权重加载逻辑详解

### 4.1 量化配置传递路径

```
CLI: --quantization-config '{"method":"gguf","gguf_model":"/path/model.gguf"}'
  │
  ▼
argparse: json.loads() → dict
  │
  ▼
OmniDiffusionConfig.quantization_config = dict
  │
  ▼
OmniDiffusionConfig.__post_init__()
  │
  ├── 如果是 str → build_quant_config(str)
  ├── 如果是 dict → build_quant_config(dict)
  │     │
  │     └── 根据 method 字段创建对应的 QuantizationConfig
  │           ├── "fp8" → Fp8Config
  │           ├── "gguf" → DiffusionGGUFConfig
  │           ├── "awq" → AWQConfig
  │           └── ...
  │
  └── 如果是 QuantizationConfig → 直接使用

同时，从模型 config 自动检测：
  └── tf_model_config.quant_config → 如果存在，自动设置 quantization_config
```

### 4.2 量化模型加载流程

**文件**：`vllm_omni/diffusion/model_loader/diffusers_loader.py`

```
DiffusersPipelineLoader.load_model(od_config, load_device, load_format, device)
  │
  ├── 检测量化 + CPU offload:
  │     └── if load_device == "cpu" and od_config.quantization_config is not None:
  │           load_device = device.type  # 在 GPU 上加载权重以进行 FP8 量化
  │
  ├── with set_default_torch_dtype(od_config.dtype):
  │     │
  │     ├── initialize_model(od_config)   # 创建模型结构（在 target_device 上）
  │     │
  │     ├── 判断量化类型:
  │     │     ├── 如果是 GGUF 量化:
  │     │     │     └── _load_weights_with_gguf(model, od_config)
  │     │     │           │
  │     │     │           ├── for source in weights_sources:
  │     │     │           │     ├── 如果是 transformer source:
  │     │     │           │     │     ├── 从 GGUF 文件加载量化权重
  │     │     │           │     │     │     └── _get_gguf_weights_iterator(source, model, od_config)
  │     │     │           │     │     │           │
  │     │     │           │     │     │           ├── 解析 GGUF 文件路径
  │     │     │           │     │     │           ├── 获取 GGUF adapter
  │     │     │           │     │     │           └── adapter.weights_iterator()
  │     │     │           │     │     │
  │     │     │           │     │     └── model.load_weights(gguf_weights)
  │     │     │           │     │           │
  │     │     │           │     │           └── WanTransformer3DModel.load_weights(weights)
  │     │     │           │     │                 │
  │     │     │           │     │                 ├── QKV 融合映射（与未量化相同）
  │     │     │           │     │                 ├── 名称重映射
  │     │     │           │     │                 ├── TP 分片的 RMSNorm 权重处理
  │     │     │           │     │                 └── weight_loader(param, loaded_weight)
  │     │     │           │     │                       │
  │     │     │           │     │                       └── 对于量化层，param 有 weight_loader 属性
  │     │     │           │     │                             └── 调用量化层的自定义 weight_loader
  │     │     │           │     │
  │     │     │           │     └── 检查是否有缺失权重
  │     │     │           │           └── 如果有缺失 → 回退到 HF safetensors 加载
  │     │     │           │
  │     │     │           └── 验证所有权重已加载
  │     │     │
  │     │     └── 其他量化 (FP8/AWQ/GPTQ):
  │     │           └── load_weights(model)
  │     │                 │
  │     │                 ├── get_all_weights(model)  # safetensors 权重迭代器
  │     │                 │
  │     │                 └── model.load_weights(weights)
  │     │                       │
  │     │                       └── 与未量化模型相同的加载逻辑
  │     │                             （QKV 融合、名称映射、TP 分片等）
  │     │
  │     └── _process_weights_after_loading(model, target_device)
  │           │
  │           └── for module in model.named_modules():
  │                 quant_method = getattr(module, "quant_method", None)
  │                 │
  │                 └── if isinstance(quant_method, QuantizeMethodBase):
  │                       │
  │                       ├── module.to(target_device)    # 移动到目标设备
  │                       │
  │                       ├── quant_method.process_weights_after_loading(module)
  │                       │     │
  │                       │     └── 对于 FP8 量化:
  │                       │           ├── 计算 weight_scale（基于权重分布）
  │                       │           ├── 将 BF16/FP16 权重转换为 FP8
  │                       │           └── 注册 weight_scale 为 buffer
  │                       │
  │                       └── module.to(module_device)    # 移回原设备
  │
  └── return model.eval()
```

### 4.3 量化权重缺失的容错处理

**文件**：`vllm_omni/diffusion/model_loader/diffusers_loader.py`

```python
# 这些后缀的权重是量化方法在模型中注册的，但 checkpoint 中不存在
_QUANTIZED_WEIGHT_SUFFIXES = (
    ".g_idx",           # GPTQ / AWQ / AutoRound 的 g_idx（可选）
    ".weight_scale",    # FP8 权重缩放因子
    ".weight_scale_inv",# FP8 逐 token 缩放因子
    ".input_scale",     # FP8 激活缩放因子
    ".qweight_type",    # GGUF 量化类型
)

def _check_unloaded_weights(weights_not_loaded):
    if od_config.quantization_config is None:
        # 未量化模型：任何权重缺失都报错
        raise ValueError(f"Following weights were not initialized: {weights_not_loaded}")

    # 量化模型：区分预期缺失和意外缺失
    expected_missing = {w for w in weights_not_loaded if _is_expected_quantized_weight(w)}
    unexpected_missing = weights_not_loaded - expected_missing

    if expected_missing:
        logger.warning("Following weights were not initialized (expected for quantized models): %s", expected_missing)
    if unexpected_missing:
        raise ValueError(f"Following weights were not initialized: {unexpected_missing}")
```

### 4.4 Wan2.2 Transformer 的 load_weights() 详解

**文件**：`vllm_omni/diffusion/models/wan2_2/wan2_2_transformer.py`

```python
def load_weights(self, weights: Iterable[tuple[str, torch.Tensor]]) -> set[str]:
    tp_rank = get_tensor_model_parallel_rank()
    tp_size = get_tensor_model_parallel_world_size()

    # 1. QKV 融合映射（仅自注意力 attn1）
    stacked_params_mapping = [
        (".attn1.to_qkv", ".attn1.to_q", "q"),  # 将 to_q 权重加载到 to_qkv 的 q 部分
        (".attn1.to_qkv", ".attn1.to_k", "k"),  # 将 to_k 权重加载到 to_qkv 的 k 部分
        (".attn1.to_qkv", ".attn1.to_v", "v"),  # 将 to_v 权重加载到 to_qkv 的 v 部分
    ]

    # 2. 名称重映射
    weight_name_remapping = {
        "scale_shift_table": "output_scale_shift_prepare.scale_shift_table",
    }

    params_dict = dict(self.named_parameters())
    loaded_params: set[str] = set()

    for name, loaded_weight in weights:
        name = weight_name_remapping.get(name, name)

        # 3. 处理 QKV 融合
        for param_name, weight_name, shard_id in stacked_params_mapping:
            if weight_name in name:
                lookup_name = name.replace(weight_name, param_name)
                param = params_dict[lookup_name]
                weight_loader = param.weight_loader  # QKVParallelLinear 的自定义 weight_loader
                weight_loader(param, loaded_weight, shard_id)  # 按 shard_id 加载到对应位置
                break
        else:
            # 4. 名称格式转换
            if ".ffn.net.0." in name:
                name = name.replace(".ffn.net.0.", ".ffn.net_0.")
            elif ".ffn.net.2." in name:
                name = name.replace(".ffn.net.2.", ".ffn.net_2.")
            if ".to_out.0." in name:
                name = name.replace(".to_out.0.", ".to_out.")

            if name not in params_dict:
                logger.warning(f"Skipping weight {name}")
                continue

            param = params_dict[name]

            # 5. TP 分片的 RMSNorm 权重处理
            # RMSNorm 应用在 ColumnParallelLinear 输出后，权重需要按 TP rank 分片
            if tp_size > 1 and any(norm_name in name for norm_name in [
                ".attn1.norm_q.", ".attn1.norm_k.",
                ".attn2.norm_q.", ".attn2.norm_k.",
                ".attn2.norm_added_k.",
            ]):
                shard_size = loaded_weight.shape[0] // tp_size
                loaded_weight = loaded_weight[tp_rank * shard_size : (tp_rank + 1) * shard_size]

            # 6. 加载权重
            weight_loader = getattr(param, "weight_loader", default_weight_loader)
            weight_loader(param, loaded_weight)

        loaded_params.add(name)

    return loaded_params
```

---

## 5. 关键数据结构

### 5.1 OmniDiffusionConfig

```python
@dataclass
class OmniDiffusionConfig:
    model: str                          # 模型路径
    model_class_name: str               # Pipeline 类名（如 "WanPipeline"）
    dtype: torch.dtype                  # 数据类型（默认 bf16）
    boundary_ratio: float | None        # 双 transformer 切换比例（Wan2.2 默认 0.875）
    flow_shift: float | None            # 调度器 flow shift（720p=5.0, 480p=12.0）

    parallel_config: DiffusionParallelConfig
        tensor_parallel_size: int       # TP 大小
        ulysses_degree: int             # Ulysses SP 程度
        ring_degree: int                # Ring SP 程度
        cfg_parallel_size: int          # CFG 并行大小（1 或 2）
        use_hsdp: bool                  # 是否启用 HSDP
        hsdp_shard_size: int            # HSDP 分片大小
        hsdp_replicate_size: int        # HSDP 副本数

    quantization_config: QuantizationConfig | None  # 量化配置
    enable_cpu_offload: bool            # 是否启用 CPU offload
    enable_layerwise_offload: bool      # 是否启用逐层 offload
    cache_backend: str                  # 缓存后端（"none"/"cache_dit"/"tea_cache"）
    step_execution: bool                # 是否启用逐步执行
    ...
```

### 5.2 Wan22Pipeline 关键属性

```python
class Wan22Pipeline(nn.Module, CFGParallelMixin, ProgressBarMixin, DiffusionPipelineProfilerMixin):
    # 组件
    tokenizer: AutoTokenizer            # 分词器
    text_encoder: UMT5EncoderModel      # 文本编码器
    vae: DistributedAutoencoderKLWan    # VAE
    transformer: WanTransformer3DModel  # 高噪声阶段 transformer
    transformer_2: WanTransformer3DModel | None  # 低噪声阶段 transformer（MoE 模型）
    scheduler: FlowUniPCMultistepScheduler  # 调度器

    # 配置
    boundary_ratio: float               # 切换比例
    expand_timesteps: bool              # 是否启用 expand_timesteps 模式（TI2V）
    has_transformer_2: bool             # 是否有第二个 transformer

    # 权重来源
    weights_sources: list[ComponentSource]  # 用于 load_weights
```

---

## 6. 进程/线程模型

```
主进程 (API Server)
  │
  ├── AsyncOmni (主线程)
  │     └── 通过 janus 队列与 Orchestrator 线程通信
  │
  └── Orchestrator 线程
        │
        ├── asyncio 事件循环
        │
        ├── StageEngineCoreClient (LLM stages, 子进程)
        │     └── vLLM EngineCore 子进程
        │
        └── StageDiffusionClient (Diffusion stages)
              │
              └── ZMQ → StageDiffusionProc 子进程
                    │
                    ├── DiffusionEngine
                    │     │
                    │     └── MultiprocDiffusionExecutor
                    │           │
                    │           ├── WorkerProc 子进程 (rank 0)
                    │           │     ├── DiffusionWorker
                    │           │     │     └── DiffusionModelRunner
                    │           │     │           └── Wan22Pipeline (模型)
                    │           │     └── MessageQueue (结果发送者)
                    │           │
                    │           ├── WorkerProc 子进程 (rank 1)
                    │           │     └── ...
                    │           │
                    │           └── ... (更多 GPU 进程)
                    │
                    └── MessageQueue (广播接收器)
```

---

## 7. 关键设计要点

1. **多层进程隔离**：API Server → Orchestrator 线程 → StageDiffusionProc → WorkerProc，每层都有独立的进程/线程
2. **ZMQ + MessageQueue 通信**：StageDiffusionClient 与 Proc 之间用 ZMQ，Executor 与 Worker 之间用共享内存 MessageQueue
3. **权重加载与量化分离**：先加载 BF16/FP16 权重，再通过 `process_weights_after_loading()` 进行量化转换
4. **QKV 融合**：Diffusers 的分离 Q/K/V 权重在加载时融合为 vLLM 的 `QKVParallelLinear`
5. **TP 感知的 RMSNorm**：`DistributedRMSNorm` 在 TP 下计算全局 RMS，确保数值正确性
6. **Boundary Ratio**：Wan2.2 MoE 模型通过 `boundary_ratio` 控制高低噪声阶段的 transformer 切换
7. **CPU Offload + 量化**：启用 CPU offload 时，权重先在 GPU 上加载以进行量化，然后再 offload 到 CPU
