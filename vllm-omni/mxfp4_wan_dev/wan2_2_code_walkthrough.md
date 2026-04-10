# Wan2.2 模型代码走读文档

## 1. 概述

Wan2.2 是 vLLM-Omni 中支持的视频生成扩散模型，包含 4 种 Pipeline 变体：

| Pipeline | 文件 | 功能 |
|----------|------|------|
| `Wan22Pipeline` | `pipeline_wan2_2.py` | T2V（文本生成视频） |
| `Wan22I2VPipeline` | `pipeline_wan2_2_i2v.py` | I2V（图像生成视频，MoE 双 transformer） |
| `Wan22TI2VPipeline` | `pipeline_wan2_2_ti2v.py` | TI2V（文本+图像生成视频，单 transformer，expand_timesteps 模式） |
| `Wan22VACEPipeline` | `pipeline_wan2_2_vace.py` | VACE（视频创建/编辑全能模型） |

核心代码目录：`vllm_omni/diffusion/models/wan2_2/`

---

## 2. 完整调用链路

```
服务启动
  │
  ▼
DiffusersPipelineLoader.load_model()          # 模型加载入口
  │
  ├── initialize_model(od_config)             # 根据 registry 实例化 Pipeline
  │     │
  │     └── Wan22Pipeline.__init__()          # 初始化各组件
  │           ├── AutoTokenizer.from_pretrained()     # 分词器
  │           ├── UMT5EncoderModel.from_pretrained()  # 文本编码器
  │           ├── DistributedAutoencoderKLWan.from_pretrained()  # VAE
  │           ├── load_transformer_config()             # 读取 transformer config.json
  │           ├── create_transformer_from_config()      # 实例化 WanTransformer3DModel
  │           └── FlowUniPCMultistepScheduler()         # 调度器
  │
  ├── DiffusersPipelineLoader.load_weights()  # 加载权重
  │     │
  │     ├── get_all_weights(model)            # 从 weights_sources 获取权重迭代器
  │     │     └── _get_weights_iterator()     # safetensors 权重加载（支持多线程）
  │     │
  │     └── model.load_weights(weights)       # 调用 Wan22Pipeline.load_weights()
  │           └── AutoWeightsLoader.load_weights()
  │                 └── WanTransformer3DModel.load_weights()  # 处理 QKV 融合等映射
  │
  └── _process_weights_after_loading()        # 量化后处理（如 FP8）

推理请求
  │
  ▼
Wan22Pipeline.forward()                       # 推理主入口
  │
  ├── encode_prompt()                         # 文本编码（UMT5）
  │     ├── tokenizer()
  │     └── text_encoder()
  │
  ├── prepare_latents()                       # 准备初始噪声潜变量
  │
  └── 去噪循环 (for t in timesteps)
        │
        ├── 根据 boundary_timestep 选择 transformer 或 transformer_2
        │
        ├── predict_noise_maybe_with_cfg()    # 可能含 CFG 并行
        │     └── predict_noise()
        │           └── current_model.forward()  # WanTransformer3DModel.forward()
        │                 │
        │                 ├── rope()            # 旋转位置编码
        │                 ├── patch_embedding() # 3D 卷积 patch 嵌入
        │                 ├── condition_embedder()  # 时间/文本/图像条件嵌入
        │                 ├── timestep_proj_prepare()
        │                 │
        │                 └── for block in blocks:  # Transformer 块循环
        │                       └── WanTransformerBlock.forward()
        │                             ├── attn1 (自注意力)
        │                             │     ├── to_qkv (QKV 并行线性)
        │                             │     ├── norm_q / norm_k (DistributedRMSNorm)
        │                             │     ├── apply_rotary_emb_wan()
        │                             │     └── attn (vLLM Attention)
        │                             │
        │                             ├── attn2 (交叉注意力)
        │                             │     ├── to_q / to_k / to_v (ColumnParallelLinear)
        │                             │     ├── add_k_proj / add_v_proj (I2V 图像 KV)
        │                             │     └── attn
        │                             │
        │                             └── ffn (前馈网络)
        │                                   ├── net_0 (ColumnParallelGELU)
        │                                   └── net_2 (RowParallelLinear)
        │
        │                 ├── output_scale_shift_prepare()
        │                 ├── proj_out()
        │                 └── unpatchify()      # 反 patch 化
        │
        └── scheduler_step_maybe_with_cfg()   # 调度器步进

  └── vae.decode()                            # VAE 解码潜变量→视频
```

---

## 3. 核心文件与函数详解

### 3.1 `pipeline_wan2_2.py` — T2V Pipeline

#### 辅助函数

| 函数 | 行号 | 功能 |
|------|------|------|
| `retrieve_latents()` | 37-50 | 从 VAE 编码器输出中提取潜变量，支持 sample/argmax 模式 |
| `load_transformer_config()` | 53-73 | 从本地或 HF Hub 加载 transformer 的 config.json |
| `create_transformer_from_config()` | 76-111 | 根据 config dict 创建 `WanTransformer3DModel` |
| `get_wan22_pre_process_func()` | 132-190 | 请求预处理：加载/缩放输入图像（I2V 模式） |
| `get_wan22_post_process_func()` | 114-129 | 后处理：将潜变量解码为 numpy/视频格式 |

#### `Wan22Pipeline` 类

| 方法 | 行号 | 功能 |
|------|------|------|
| `__init__()` | 194-317 | **Pipeline 初始化**：读取 model_index.json 检测 expand_timesteps 和 transformer_2；根据 boundary_ratio 决定加载哪些 transformer；初始化 tokenizer、text_encoder、VAE、scheduler |
| `_create_transformer()` | 319-321 | 从 config 创建 transformer（子类可重写） |
| `forward()` | 339-713 | **推理主函数**：参数校验→文本编码→准备潜变量→去噪循环→VAE 解码 |
| `predict_noise()` | 715-728 | 调用 transformer 前向传播预测噪声 |
| `encode_prompt()` | 730-798 | 使用 UMT5 编码文本 prompt 和 negative prompt |
| `prepare_latents()` | 804-830 | 生成或复用初始噪声潜变量 |
| `load_weights()` | 832-835 | 使用 `AutoWeightsLoader` 加载权重 |
| `check_inputs()` | 837-874 | 输入参数校验 |

**关键逻辑 — 双 transformer 选择（MoE 架构）**：
```python
# 根据 timestep 和 boundary_ratio 选择模型
if boundary_timestep is not None and t < boundary_timestep:
    current_model = self.transformer_2  # 低噪声阶段
    current_guidance_scale = guidance_high
else:
    current_model = self.transformer    # 高噪声阶段
    current_guidance_scale = guidance_low
```

**关键逻辑 — I2V expand_timesteps 模式**：
```python
# I2V 模式：将图像条件与潜变量混合
latent_model_input = (1 - first_frame_mask) * latent_condition + first_frame_mask * latents
# 每个 patch 有不同的 timestep
timestep = (patch_mask[0][0] * t).flatten()
```

---

### 3.2 `wan2_2_transformer.py` — 核心 Transformer

#### 模块级函数

| 函数 | 行号 | 功能 |
|------|------|------|
| `apply_rotary_emb_wan()` | 37-63 | 对输入张量应用旋转位置编码（RoPE），将复数旋转拆分为实数运算 |

#### 核心模块类

| 类 | 行号 | 功能 |
|----|------|------|
| `DistributedRMSNorm` | 66-96 | **TP 感知的 RMSNorm**：在 tensor parallel 下计算全局 RMS，确保与非 TP 执行数学等价 |
| `ColumnParallelGELU` | 99-115 | ColumnParallelLinear + GELU 激活 |
| `WanFeedForward` | 118-151 | **TP 前馈网络**：ColumnParallel(GELU) → Identity → RowParallel |
| `WanRotaryPosEmbed` | 154-229 | **3D 旋转位置编码**：为时间/高度/宽度三个维度分别生成 RoPE 频率 |
| `WanImageEmbedding` | 232-255 | I2V 任务的图像嵌入模块 |
| `WanTimeTextImageEmbedding` | 258-303 | 组合时间步、文本、图像条件嵌入 |
| `TimestepProjPrepare` | 306-323 | 为 SP 准备 timestep_proj（TI2V 模式下的 4D tensor unflatten） |
| `OutputScaleShiftPrepare` | 326-344 | 为 SP 准备输出 scale/shift |
| `WanSelfAttention` | 347-443 | **自注意力**：使用 vLLM 的 `QKVParallelLinear` 融合 QKV，`DistributedRMSNorm` 做 QK 归一化，`Attention` 统一注意力层 |
| `WanCrossAttention` | 446-600 | **交叉注意力**：Q 来自 hidden_states，K/V 来自 encoder；支持 I2V 的 added_kv_proj（图像 KV 投影） |
| `WanTransformerBlock` | 603-690 | **Transformer 块**：自注意力 → 交叉注意力 → FFN，使用 timestep 的 scale-shift 调制 |

#### `WanTransformer3DModel` 类（主模型）

| 方法/属性 | 行号 | 功能 |
|-----------|------|------|
| `_sp_plan` | 750-781 | **序列并行计划**：定义 RoPE、hidden_states、timestep_proj、输出的 sharding/gathering 策略 |
| `__init__()` | 784-862 | 初始化 patch_embedding、condition_embedder、transformer blocks、输出投影 |
| `forward()` | 869-957 | **前向传播**：RoPE → patch 嵌入 → 条件嵌入 → transformer blocks → 输出投影 → unpatchify |
| `load_weights()` | 959-1045 | **权重加载**：处理 QKV 融合映射、名称重映射、TP 分片 |

**`forward()` 详细流程**：
```
1. rope(hidden_states)                    → 获取 freqs_cos, freqs_sin
2. patch_embedding(hidden_states)         → 3D 卷积 → flatten → transpose
3. condition_embedder(timestep, text, image) → 获取 temb, timestep_proj, encoder_hidden_states
4. timestep_proj_prepare(timestep_proj)   → unflatten 为 [B, seq, 6, dim] 或 [B, 6, dim]
5. for block in blocks:
     block(hidden_states, encoder_hidden_states, timestep_proj, rotary_emb, mask)
6. output_scale_shift_prepare(temb)       → 获取 shift, scale
7. norm_out → proj_out → unpatchify       → 输出视频潜变量
```

---

### 3.3 `diffusers_loader.py` — 模型加载器

| 方法 | 行号 | 功能 |
|------|------|------|
| `load_model()` | 256-294 | **加载入口**：设置 dtype→初始化模型→加载权重→量化后处理 |
| `load_weights()` | 319-342 | 调用模型的 `load_weights()` 方法，检查未加载的权重 |
| `_get_weights_iterator()` | 175-211 | 获取权重迭代器（支持 safetensors 单线程/多线程加载） |
| `_process_weights_after_loading()` | 296-317 | **量化后处理**：遍历模块调用 `quant_method.process_weights_after_loading()` |
| `_is_gguf_quantization()` | 392-424 | 检测是否为 GGUF 量化格式 |
| `_load_weights_with_gguf()` | 480-510 | GGUF 权重加载（支持 GGUF+HF 混合加载） |
| `_load_model_with_hsdp()` | 512-564 | HSDP 分片模型加载 |

---

## 4. 量化权重加载逻辑

### 4.1 量化配置检测

在 `OmniDiffusionConfig` 初始化时（`data.py`）自动检测量化配置：

```python
# 1. 从命令行参数获取
quantization_config = params.get("quantization_config")

# 2. 从模型 config 自动检测
if self.quantization_config is None and self.tf_model_config.quant_config is not None:
    self.quantization_config = self.tf_model_config.quant_config

# 3. 解析为 QuantizationConfig 对象
if isinstance(self.quantization_config, str):
    self.quantization_config = build_quant_config(self.quantization_config)
elif isinstance(self.quantization_config, Mapping):
    self.quantization_config = build_quant_config(dict(self.quantization_config))
```

### 4.2 加载流程

```
DiffusersPipelineLoader.load_model()
  │
  ├── 检测量化: od_config.quantization_config is not None
  │     └── 如果是 CPU offload + 量化 → 在 GPU 上加载权重（用于 FP8 量化）
  │
  ├── initialize_model(od_config)       # 在 target_device 上初始化模型
  │
  ├── 判断量化类型:
  │     ├── GGUF 量化 → _load_weights_with_gguf()
  │     │     ├── 使用 GGUF adapter 读取量化权重
  │     │     └── 如有缺失权重 → 回退到 HF safetensors 加载
  │     │
  │     └── 其他量化 (FP8/AWQ/GPTQ) → load_weights()
  │           └── model.load_weights(weights)
  │
  └── _process_weights_after_loading()  # 量化后处理
        └── 遍历所有模块，调用 quant_method.process_weights_after_loading()
              （例如 FP8 在线量化：将 BF16/FP16 权重转换为 FP8）
```

### 4.3 Wan2.2 的 `load_weights()` 量化处理

`WanTransformer3DModel.load_weights()` 处理以下量化相关逻辑：

**1. QKV 融合映射**：
```python
stacked_params_mapping = [
    (".attn1.to_qkv", ".attn1.to_q", "q"),  # 融合 Q
    (".attn1.to_qkv", ".attn1.to_k", "k"),  # 融合 K
    (".attn1.to_qkv", ".attn1.to_v", "v"),  # 融合 V
]
```
Diffusers 的分离 Q/K/V 权重被融合到 vLLM 的 `QKVParallelLinear` 中。

**2. 权重名称重映射**：
```python
weight_name_remapping = {
    "scale_shift_table": "output_scale_shift_prepare.scale_shift_table",
}
```

**3. 名称格式转换**：
```python
# diffusers: ffn.net.0.proj.weight → vllm-omni: ffn.net_0.proj.weight
# diffusers: to_out.0.weight     → vllm-omni: to_out.weight
```

**4. TP 分片的 RMSNorm 权重处理**：
```python
# RMSNorm 应用在 ColumnParallelLinear 输出后，权重需要按 TP rank 分片
if tp_size > 1 and any(norm_name in lookup_name for norm_name in [
    ".attn1.norm_q.", ".attn1.norm_k.",
    ".attn2.norm_q.", ".attn2.norm_k.",
    ".attn2.norm_added_k.",
]):
    shard_size = loaded_weight.shape[0] // tp_size
    loaded_weight = loaded_weight[tp_rank * shard_size : (tp_rank + 1) * shard_size]
```

### 4.4 量化权重未加载的容错

`DiffusersPipelineLoader._check_unloaded_weights()` 对量化模型的缺失权重做特殊处理：

```python
# 这些后缀的权重是量化方法在模型中注册的，但 checkpoint 中不存在
_QUANTIZED_WEIGHT_SUFFIXES = (
    ".g_idx",           # GPTQ / AWQ / AutoRound
    ".weight_scale",    # FP8
    ".weight_scale_inv",# FP8
    ".input_scale",     # FP8
    ".qweight_type",    # GGUF
)

# 量化模型缺失这些权重是预期的，仅打印 warning
# 其他权重缺失则抛出 ValueError
```

### 4.5 FP8 量化后处理

`_process_weights_after_loading()` 在权重加载完成后执行：

```python
for _, module in model.named_modules():
    quant_method = getattr(module, "quant_method", None)
    if isinstance(quant_method, QuantizeMethodBase):
        # 移动到目标设备
        module.to(target_device)
        # 执行量化后处理（如计算 weight_scale、转换权重为 FP8）
        quant_method.process_weights_after_loading(module)
        # 移回原设备
        module.to(module_device)
```

### 4.6 GGUF 量化加载

GGUF 量化支持混合加载（GGUF 量化权重 + HF 未量化权重）：

```python
def _load_weights_with_gguf():
    for source in sources:
        if self._is_transformer_source(source):
            # 1. 从 GGUF 文件加载量化权重
            loaded |= model.load_weights(self._get_gguf_weights_iterator(...))

            # 2. 检查是否有缺失权重
            has_missing = any(name not in loaded for name in loadable_names)

            # 3. 如有缺失，回退到 HF safetensors 加载
            if has_missing:
                hf_iter = self._get_weights_iterator(source)
                loaded |= model.load_weights(hf_iter)
        else:
            # 非 transformer 组件（VAE、text_encoder）直接从 HF 加载
            loaded |= model.load_weights(self._get_weights_iterator(source))
```

---

## 5. 分布式并行策略

| 策略 | 说明 | 实现位置 |
|------|------|----------|
| **Tensor Parallel (TP)** | QKV/FFN 层分片 | `QKVParallelLinear`, `ColumnParallelLinear`, `RowParallelLinear` |
| **Sequence Parallel (SP)** | 序列维度分片（等同 diffusers 的 Context Parallel） | `_sp_plan` 定义 sharding 策略 |
| **CFG Parallel** | 正负 prompt 并行推理 | `CFGParallelMixin` |
| **HSDP** | Hybrid Sharded Data Parallel | `apply_hsdp_to_model()` |
| **Ring Attention** | 环形注意力 | `ring_selector.py` |
| **VAE Patch Parallel** | VAE 分块并行 | `DistributedAutoencoderKLWan` |

---

## 6. 关键设计要点

1. **Lazy Loading**：`Wan22Pipeline` 中 `Omni`/`AsyncOmni` 通过 `__getattr__` 懒加载，避免导入重型依赖
2. **Boundary Ratio**：双 transformer 模型通过 `boundary_ratio` 控制高低噪声阶段的切换点
3. **Expand Timesteps**：TI2V 模式下，每个 patch 有不同的 timestep，实现图像条件注入
4. **QKV Fusion**：自注意力的 Q/K/V 融合为单个 `QKVParallelLinear`，提升 TP 效率
5. **Distributed RMSNorm**：TP 下计算全局 RMS，确保数值正确性
6. **SP Auto Pad**：序列并行支持动态 padding 和 attention mask 生成
