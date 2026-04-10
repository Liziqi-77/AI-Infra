# vLLM-Omni 量化框架架构详解

> 本文档为开发 MXFP4 适配 NPU 和 GPU 特性提供技术参考，以 WAN2.2 作为开发模型。

## 一、量化框架核心架构概览

```
┌─────────────────────────────────────────────────────────────────────┐
│                         用户配置入口                                  │
│  quantization="fp8" / quantization_config={"method": "int8"}        │
└─────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
┌─────────────────────────────────────────────────────────────────────┐
│                    量化配置工厂 (factory.py)                          │
│  build_quant_config() → 解析配置 → 返回 QuantizationConfig           │
└─────────────────────────────────────────────────────────────────────┘
                                    │
                    ┌───────────────┼───────────────┐
                    ▼               ▼               ▼
┌──────────────────┐ ┌──────────────────┐ ┌──────────────────┐
│ vLLM原生量化方法  │ │ OMNI扩展量化方法  │ │ 组件级量化路由    │
│ (FP8/GPTQ/AWQ等) │ │ (INT8/GGUF/INC)  │ │ ComponentConfig  │
└──────────────────┘ └──────────────────┘ └──────────────────┘
                                    │
                                    ▼
┌─────────────────────────────────────────────────────────────────────┐
│                    平台适配层 (platforms/)                           │
│  current_omni_platform.is_cuda() / is_npu() → 选择量化方法实现       │
└─────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
┌─────────────────────────────────────────────────────────────────────┐
│                    量化方法实现 (LinearMethod)                        │
│  create_weights() → process_weights_after_loading() → apply()       │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 二、关键文件路径清单

### 1. 量化核心模块

| 文件路径 | 功能说明 |
|---------|---------|
| `vllm_omni/quantization/__init__.py` | 模块入口，导出 `build_quant_config` |
| `vllm_omni/quantization/factory.py` | **核心工厂函数**，构建量化配置 |
| `vllm_omni/quantization/component_config.py` | 组件级量化路由配置 |
| `vllm_omni/quantization/int8_config.py` | **INT8量化实现**（CUDA+NPU双平台） |
| `vllm_omni/quantization/gguf_config.py` | GGUF量化实现 |

### 2. 平台适配层

| 文件路径 | 功能说明 |
|---------|---------|
| `vllm_omni/platforms/__init__.py` | 平台自动检测和初始化 |
| `vllm_omni/platforms/interface.py` | **OmniPlatform 抽象基类** |
| `vllm_omni/platforms/cuda/platform.py` | CUDA/GPU 平台实现 |
| `vllm_omni/platforms/npu/platform.py` | **NPU/Ascend 平台实现** |

### 3. WAN2.2 模型相关

| 文件路径 | 功能说明 |
|---------|---------|
| `vllm_omni/diffusion/models/wan2_2/wan2_2_transformer.py` | **WAN2.2 Transformer 实现** |
| `vllm_omni/diffusion/models/wan2_2/pipeline_wan2_2.py` | WAN2.2 推理管道 |
| `vllm_omni/diffusion/models/wan2_2/pipeline_wan2_2_i2v.py` | WAN2.2 图生视频管道 |
| `vllm_omni/diffusion/models/wan2_2/pipeline_wan2_2_ti2v.py` | WAN2.2 文图生视频管道 |

### 4. 配置与数据流

| 文件路径 | 功能说明 |
|---------|---------|
| `vllm_omni/diffusion/data.py` | **OmniDiffusionConfig**，量化配置入口 |
| `vllm_omni/engine/arg_utils.py` | 引擎参数解析 |

### 5. 测试文件

| 文件路径 | 功能说明 |
|---------|---------|
| `tests/diffusion/quantization/test_int8_config.py` | INT8配置测试模式 |
| `tests/diffusion/quantization/test_fp8_config.py` | FP8配置测试模式 |
| `tests/diffusion/quantization/test_gguf_config.py` | GGUF配置测试模式 |

---

## 三、开发 MXFP4 特性的技术要点

### 1. 量化配置类设计模式

参考 `int8_config.py` 的实现模式：

```python
class DiffusionMXFP4Config(QuantizationConfig):
    """MXFP4量化配置，支持CUDA和NPU平台"""
    
    def __init__(
        self,
        is_checkpoint_mxfp4_serialized: bool = False,  # 是否预量化检查点
        activation_scheme: str = "dynamic",             # 激活量化方案
        ignored_layers: list[str] | None = None,       # 跳过的敏感层
    ) -> None:
        super().__init__()
        self.is_checkpoint_mxfp4_serialized = is_checkpoint_mxfp4_serialized
        self.activation_scheme = activation_scheme
        self.ignored_layers = ignored_layers or []

    @classmethod
    def get_name(cls) -> QuantizationMethods:
        return "mxfp4"

    @classmethod
    def get_supported_act_dtypes(cls) -> list[torch.dtype]:
        return [torch.bfloat16, torch.float16]

    @classmethod
    def get_min_capability(cls) -> int:
        return 80  # 最低硬件能力要求

    def get_quant_method(
        self,
        layer: torch.nn.Module,
        prefix: str,
    ) -> Optional["QuantizeMethodBase"]:
        if isinstance(layer, LinearBase):
            if is_layer_skipped(prefix, self.ignored_layers, ...):
                return UnquantizedLinearMethod()
            
            # 关键：根据平台选择不同的量化方法
            if current_omni_platform.is_cuda():
                return MXFP4LinearMethod(self)      # CUDA实现
            elif current_omni_platform.is_npu():
                return NPU_MXFP4LinearMethod(self)  # NPU实现
        return None
```

### 2. 平台适配层接口

在 `interface.py` 中，`OmniPlatform` 提供了平台检测方法：

```python
from vllm_omni.platforms import current_omni_platform

# 平台检测
if current_omni_platform.is_cuda():
    # GPU/CUDA 特定实现
    pass
elif current_omni_platform.is_npu():
    # NPU/Ascend 特定实现
    pass
```

**OmniPlatform 关键方法：**

| 方法 | 说明 |
|------|------|
| `is_cuda()` | 检测是否为 CUDA 平台 |
| `is_npu()` | 检测是否为 NPU/Ascend 平台 |
| `is_xpu()` | 检测是否为 Intel XPU 平台 |
| `is_rocm()` | 检测是否为 AMD ROCm 平台 |
| `supports_float64()` | 检测是否支持 float64 |

### 3. 量化方法实现模式

参考 INT8 的实现，量化方法需要实现三个核心方法：

```python
class MXFP4LinearMethod(LinearMethodBase):
    """MXFP4线性层量化方法"""
    
    def create_weights(
        self,
        layer: torch.nn.Module,
        input_size_per_partition: int,
        output_partition_sizes: list[int],
        input_size: int,
        output_size: int,
        params_dtype: torch.dtype,
        **extra_weight_attrs,
    ):
        """创建量化权重参数"""
        # 定义 weight, weight_scale 等参数
        layer.logical_widths = output_partition_sizes
        layer.input_size_per_partition = input_size_per_partition
        layer.output_size_per_partition = sum(output_partition_sizes)
        
        # 创建量化权重参数
        weight = ModelWeightParameter(
            data=torch.empty(
                output_size_per_partition,
                input_size_per_partition,
                dtype=params_dtype,
            ),
            input_dim=1,
            output_dim=0,
            weight_loader=weight_loader,
        )
        layer.register_parameter("weight", weight)
        
        # 创建缩放因子参数
        scale = ChannelQuantScaleParameter(
            data=torch.empty((sum(output_partition_sizes), 1), dtype=torch.float32),
            output_dim=0,
            weight_loader=weight_loader,
        )
        layer.register_parameter("weight_scale", scale)
        
    def process_weights_after_loading(self, layer: Module) -> None:
        """权重加载后处理：执行量化转换"""
        # 将 BF16/FP16 权重量化为 MXFP4
        qweight, weight_scale = quantize_to_mxfp4(layer.weight)
        
        # 更新层参数
        replace_parameter(layer, "weight", qweight)
        replace_parameter(layer, "weight_scale", weight_scale)
        
    def apply(
        self,
        layer: torch.nn.Module,
        x: torch.Tensor,
        bias: torch.Tensor | None = None,
    ) -> torch.Tensor:
        """前向传播：执行量化矩阵乘法"""
        # 实现 MXFP4 量化矩阵乘法
        output = mxfp4_matmul(x, layer.weight, layer.weight_scale)
        if bias is not None:
            output = output + bias
        return output
```

### 4. 在 factory.py 中注册新量化方法

在 `factory.py` 中添加：

```python
def _build_mxfp4(**kw: Any) -> QuantizationConfig:
    """Lazy import for MXFP4 config."""
    from .mxfp4_config import DiffusionMXFP4Config
    return DiffusionMXFP4Config(**kw)

_OVERRIDES: dict[str, Callable[..., QuantizationConfig]] = {
    "gguf": _build_gguf,
    "int8": _build_int8,
    "inc": _build_inc,
    "auto-round": _build_inc,
    "mxfp4": _build_mxfp4,  # 新增
}

SUPPORTED_QUANTIZATION_METHODS: list[str] = list(dict.fromkeys(
    QUANTIZATION_METHODS + list(_OVERRIDES.keys())
))
```

### 5. WAN2.2 模型量化集成

WAN2.2 Transformer 使用 vLLM 的并行线性层，天然支持量化：

```python
# wan2_2_transformer.py 中的线性层定义
class WanSelfAttention(nn.Module):
    def __init__(self, dim: int, num_heads: int, head_dim: int, ...):
        # Fused QKV projection - 自动支持量化
        self.to_qkv = QKVParallelLinear(
            hidden_size=dim,
            head_size=head_dim,
            total_num_heads=num_heads,
            bias=True,
            # quant_config 会自动传递并应用量化
        )
        
        # Output projection - 自动支持量化
        self.to_out = RowParallelLinear(
            self.inner_dim,
            dim,
            bias=True,
            input_is_parallel=True,
            return_bias=False,
        )

class WanFeedForward(nn.Module):
    def __init__(self, dim: int, inner_dim: int, ...):
        # ColumnParallel + RowParallel 组合
        self.net_0 = ColumnParallelGELU(dim, inner_dim, ...)
        self.net_2 = RowParallelLinear(inner_dim, dim_out, ...)
```

---

## 四、NPU 平台适配要点

### 1. NPU 专用量化方法

参考 `int8_config.py` 中 NPU 的实现：

```python
class NPU_MXFP4LinearMethod(BaseMXFP4LinearMethod):
    """NPU平台的MXFP4量化方法"""
    
    def __init__(self, quant_config: DiffusionMXFP4Config):
        super().__init__(quant_config)
        import torch_npu
        self.torch_npu = torch_npu
    
    def process_weights_after_loading(self, layer: Module) -> None:
        """NPU权重处理"""
        # 使用 torch_npu 的专用量化API
        weight = layer.weight
        
        # NPU专用量化操作
        qweight, weight_scale = self.torch_npu.npu_dynamic_quant(weight)
        qweight = qweight.t().contiguous()
        
        # 更新层参数
        replace_parameter(layer, "weight", qweight)
        replace_parameter(layer, "weight_scale", weight_scale)
        
    def apply(self, layer, x, bias=None):
        """NPU前向传播"""
        ori_shape = x.shape
        ori_dtype = x.dtype
        
        x = x.reshape(-1, ori_shape[-1])
        
        # NPU专用动态量化
        quantized_x, pertoken_scale = self.torch_npu.npu_dynamic_quant(x)
        
        # NPU专用量化矩阵乘法
        output = self.torch_npu.npu_quant_matmul(
            quantized_x,
            layer.weight,
            layer.weight_scale,
            bias=bias,
            pertoken_scale=pertoken_scale,
            output_dtype=ori_dtype,
        )
        
        output = output.reshape(*ori_shape[:-1], -1)
        return output
```

### 2. NPU 平台配置文件

NPU 平台配置文件位于 `vllm_omni/platforms/npu/stage_configs/`：

```
vllm_omni/platforms/npu/stage_configs/
├── hunyuan_image3_moe_dit.yaml
├── qwen2_5_omni.yaml
├── qwen3_omni_moe.yaml
├── qwen3_omni_moe_async_chunk.yaml
└── qwen3_tts.yaml
```

可参考现有配置创建 MXFP4 专用配置。

### 3. NPU 平台特性

| 特性 | 说明 |
|------|------|
| `dist_backend` | 使用 `hccl` 作为分布式后端 |
| `autocast` | 使用 `torch.npu.amp.autocast` |
| `attention_backend` | 优先使用 `mindiesd` (Flash Attention)，回退到 SDPA |
| `torch_inductor` | 不支持 torch.compile |

---

## 五、敏感层跳过机制

某些层对量化敏感，需要跳过以保持输出质量：

```python
# 在量化配置中指定要跳过的层
quantization_config = {
    "method": "mxfp4",
    "ignored_layers": [
        "img_mlp",           # 处理去噪潜变量，动态范围变化大
        "proj_out",          # 最终输出投影，误差放大
        "blocks.0.ffn",      # 首层FFN可能敏感
    ]
}
```

### 常见敏感层及原因

| 层类型 | 敏感原因 | 典型影响 |
|--------|----------|----------|
| `img_mlp` | 处理去噪潜变量，动态范围变化大 | 色偏、模糊 |
| `feed_forward` | DiT块中的FFN层，大动态范围 | 伪影、细节丢失 |
| `proj_out` | 最终输出投影，误差放大 | 整体质量下降 |
| `lm_head` | 词汇投影，精度关键 | 文本输出错误 |
| `mlp.gate` | MoE路由门，精度关键 | 专家选择错误 |

---

## 六、组件级量化配置

支持为不同模型组件配置不同的量化策略：

```python
from vllm_omni.quantization import build_quant_config

# 为不同组件配置不同量化策略
config = build_quant_config({
    "transformer": {"method": "mxfp4"},  # Transformer使用MXFP4
    "vae": None,                          # VAE不量化
    "text_encoder": {"method": "fp8"},    # 文本编码器使用FP8
    "default": None,                      # 默认不量化
})
```

**组件级量化路由逻辑：**

```python
class ComponentQuantizationConfig(QuantizationConfig):
    """按层前缀路由量化到不同配置"""
    
    def resolve(self, prefix: str) -> QuantizationConfig | None:
        """最长前缀匹配查找配置"""
        for comp_prefix in self._sorted_prefixes:
            if prefix.startswith(comp_prefix):
                return self._components[comp_prefix]
        return self._default
```

---

## 七、在线量化 vs 离线量化

### 在线量化

加载 BF16/FP16 权重，在加载时动态量化：

```python
class MXFP4OnlineLinearMethod(LazyWeightMixin, MXFP4LinearMethod):
    """在线MXFP4量化：加载时量化"""
    
    def process_weights_after_loading(self, layer: Module) -> None:
        # 从BF16/FP16权重量化为MXFP4
        qweight, weight_scale = quantize_to_mxfp4(layer.weight)
        
        replace_parameter(layer, "weight", qweight)
        replace_parameter(layer, "weight_scale", weight_scale)
```

### 离线量化

加载预量化的 MXFP4 检查点：

```python
class MXFP4LinearMethod(BaseMXFP4LinearMethod):
    """离线MXFP4量化：加载预量化检查点"""
    
    def create_weights(self, layer, ...):
        # 直接创建INT8/MXFP4类型的权重参数
        params_dtype = torch.int8  # 或 MXFP4对应类型
        weight = create_weight_parameter(..., params_dtype=params_dtype)
        layer.register_parameter("weight", weight)
```

---

## 八、多维张量处理

扩散模型的输入可能是多维张量，需要特殊处理：

```python
# 参考 gguf_config.py 中的实现
def mxfp4_matmul_nd(x: torch.Tensor, qweight: torch.Tensor, weight_scale: torch.Tensor) -> torch.Tensor:
    """处理N-D扩散张量的MXFP4矩阵乘法"""
    ori_shape = x.shape
    ori_dtype = x.dtype
    
    # 展平为2D
    x = x.reshape(-1, ori_shape[-1])
    
    # 执行量化矩阵乘法
    output = mxfp4_matmul(x, qweight, weight_scale)
    
    # 恢复形状
    output = output.reshape(*ori_shape[:-1], -1)
    return output
```

---

## 九、开发建议

### 实现顺序

1. **先实现 CUDA 版本**：参考 INT8 的 CUDA 实现，使用 vLLM 的基础算子
2. **再适配 NPU**：参考 NPU INT8 实现，使用 `torch_npu` 的专用量化 API
3. **支持在线和离线两种模式**：在线量化优先，离线量化作为优化
4. **处理多维扩散张量**：确保前向传播支持 N-D 张量输入
5. **添加敏感层配置**：为 WAN2.2 模型确定哪些层不适合量化

### 文件创建清单

开发 MXFP4 特性需要创建以下文件：

| 文件 | 说明 |
|------|------|
| `vllm_omni/quantization/mxfp4_config.py` | MXFP4量化配置和方法实现 |
| `tests/diffusion/quantization/test_mxfp4_config.py` | MXFP4配置测试 |
| `vllm_omni/platforms/npu/stage_configs/wan2_2_mxfp4.yaml` | WAN2.2 MXFP4配置（可选） |

### 测试验证

```python
# 基本使用测试
from vllm_omni import Omni

omni = Omni(
    model="Wan2.2/Wan2.2-T2V-14B",
    quantization_config={
        "method": "mxfp4",
        "ignored_layers": ["proj_out"],
    },
)

output = omni.generate("A beautiful sunset over the ocean")
```

---

## 十、参考资源

### 内部文档

- `docs/user_guide/diffusion/quantization/overview.md` - 量化概览
- `docs/user_guide/diffusion/quantization/fp8.md` - FP8量化指南
- `docs/user_guide/diffusion/quantization/int8.md` - INT8量化指南
- `docs/contributing/model/adding_quantization_model.md` - 添加量化模型指南

### 外部参考

- [vLLM Quantization Documentation](https://docs.vllm.ai/en/latest/quantization/)
- [MXFP4 Specification (OCP)](https://www.opencompute.org/)
- [torch_npu Documentation](https://gitee.com/ascend/pytorch)
