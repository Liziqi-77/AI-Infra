# vLLM-Omni 量化模型加载与 MXFP4 量化开发指南

## 目录

- [1. 项目概述](#1-项目概述)
- [2. 安装指南](#2-安装指南)
  - [2.1 环境要求](#21-环境要求)
  - [2.2 安装步骤](#22-安装步骤)
  - [2.3 平台检测机制](#23-平台检测机制)
  - [2.4 版本管理](#24-版本管理)
- [3. 量化系统架构](#3-量化系统架构)
  - [3.1 整体架构](#31-整体架构)
  - [3.2 核心组件](#32-核心组件)
  - [3.3 支持的量化方法](#33-支持的量化方法)
- [4. 量化模型加载流程](#4-量化模型加载流程)
  - [4.1 配置构建](#41-配置构建)
  - [4.2 模型加载入口](#42-模型加载入口)
  - [4.3 权重加载与处理](#43-权重加载与处理)
- [5. 平台差异：GPU vs NPU](#5-平台差异gpu-vs-npu)
  - [5.1 平台抽象层](#51-平台抽象层)
  - [5.2 CUDA 平台实现](#52-cuda-平台实现)
  - [5.3 NPU 平台实现](#53-npu-平台实现)
  - [5.4 量化算子对比](#54-量化算子对比)
- [6. 以 INT8 为例：平台差异化量化实现模式](#6-以-int8-为例平台差异化量化实现模式)
  - [6.1 配置类设计](#61-配置类设计)
  - [6.2 Linear Method 实现](#62-linear-method-实现)
  - [6.3 平台路由机制](#63-平台路由机制)
- [7. MXFP4 量化开发指南](#7-mxfp4-量化开发指南)
  - [7.1 MXFP4 概述](#71-mxfp4-概述)
  - [7.2 现有 FP4 支持现状](#72-现有-fp4-支持现状)
  - [7.3 开发路径选择](#73-开发路径选择)
  - [7.4 从零实现 MXFP4 量化](#74-从零实现-mxfp4-量化)
  - [7.5 GPU 端 MXFP4 实现](#75-gpu-端-mxfp4-实现)
  - [7.6 NPU 端 MXFP4 实现](#76-npu-端-mxfp4-实现)
  - [7.7 注册与集成](#77-注册与集成)
- [8. 测试与验证](#8-测试与验证)
  - [8.1 单元测试](#81-单元测试)
  - [8.2 质量门控测试](#82-质量门控测试)
  - [8.3 E2E 测试](#83-e2e-测试)
- [9. 调试与常见问题](#9-调试与常见问题)
- [10. 关键文件索引](#10-关键文件索引)

---

## 1. 项目概述

vLLM-Omni 是 vLLM 的扩展框架，专注于多模态（文本、图像、视频、音频）模型的推理与服务。它支持非自回归架构（如 DiT）和异构流水线执行。

**量化系统**是 vLLM-Omni 的核心功能之一，通过统一的 `build_quant_config()` 工厂函数，将量化配置路由到 vLLM 上游注册表（35+ 方法）或 vLLM-Omni 自定义实现（INT8、GGUF、INC/AutoRound）。

---

## 2. 安装指南

### 2.1 环境要求

| 项目 | 要求 |
|------|------|
| Python | 3.10 - 3.13 |
| 构建系统 | setuptools >= 77.0.3, setuptools-scm >= 8.0 |
| 代码规范 | ruff (行长度 120) |
| 测试框架 | pytest (asyncio_mode = "auto") |

### 2.2 安装步骤

#### 开发模式安装（推荐）

```bash
# 自动检测平台并安装所有依赖（含开发工具）
pip install -e ".[dev]"

# 仅安装运行时依赖
pip install -e .
```

#### 强制指定平台

```bash
# 强制使用 CUDA 依赖
VLLM_OMNI_TARGET_DEVICE=cuda pip install -e ".[dev]"

# 强制使用 NPU 依赖
VLLM_OMNI_TARGET_DEVICE=npu pip install -e ".[dev]"
```

#### 构建 Wheel

```bash
bash scripts/build_wheel.sh --python python
```

### 2.3 平台检测机制

平台检测在 `setup.py` 中实现，优先级如下：

1. **环境变量 `VLLM_OMNI_TARGET_DEVICE`**（最高优先级）
   - 有效值：`cuda`, `rocm`, `npu`, `xpu`, `musa`, `cpu`

2. **Torch 后端自动检测**
   - CUDA: `torch.version.cuda is not None`
   - ROCm: `torch.version.hip is not None`（会自动卸载冲突的 `onnxruntime`）
   - NPU: `torch.npu.is_available()`
   - XPU: `torch.xpu.is_available()`
   - MUSA: `torch.musa.is_available()`

3. **CPU 默认回退**

```python
# setup.py 中的检测逻辑
def detect_target_device() -> str:
    # 1. 环境变量覆盖
    target_device = os.environ.get("VLLM_OMNI_TARGET_DEVICE")
    if target_device and target_device.lower() in valid_devices:
        return target_device.lower()

    # 2. Torch 后端检测
    if torch.version.cuda is not None:
        return "cuda"
    if torch.version.hip is not None:
        uninstall_onnxruntime()  # ROCm 特殊处理
        return "rocm"
    if hasattr(torch, "npu") and torch.npu.is_available():
        return "npu"
    # ... 其他平台

    # 3. CPU 回退
    return "cpu"
```

**平台依赖文件**位于 `requirements/` 目录：

| 文件 | 说明 |
|------|------|
| `common.txt` | 所有平台通用依赖 |
| `cuda.txt` | CUDA 特有依赖（onnxruntime, fa3-fwd） |
| `npu.txt` | NPU 特有依赖（onnxruntime-cann, torchaudio==2.9.0） |
| `rocm.txt` | ROCm 特有依赖 |
| `xpu.txt` | XPU 特有依赖 |
| `musa.txt` | MUSA 特有依赖 |
| `cpu.txt` | CPU 特有依赖 |

### 2.4 版本管理

版本号格式：`{基础版本}+{设备后缀}`

| 示例 | 说明 |
|------|------|
| `0.18.0+cuda` | CUDA 平台发布版本 |
| `0.18.1.dev23+g1a2b3c4.npu` | NPU 平台开发版本 |
| `0.18.0+rocm` | ROCm 平台发布版本 |

**环境变量覆盖**：
```bash
# 完全覆盖版本号
VLLM_OMNI_VERSION_OVERRIDE=0.19.0 pip install -e .
```

**注意**：CUDA 平台的版本号不添加后缀（遵循 vLLM 惯例）。

---

## 3. 量化系统架构

### 3.1 整体架构

```
用户 API (CLI/Python)
        │
        ▼
OmniDiffusionConfig.quantization_config
        │
        ▼
build_quant_config(spec)  ←── factory.py
        │
        ├── _OVERRIDES (vLLM-Omni 自定义配置)
        │       ├── gguf   → DiffusionGGUFConfig
        │       ├── int8   → DiffusionInt8Config
        │       ├── inc    → INCConfig (bits→weight_bits 映射)
        │       └── auto-round → INCConfig
        │
        └── vLLM 注册表 (QUANTIZATION_METHODS)
                ├── fp8, awq, gptq, bitsandbytes
                ├── modelopt, modelopt_fp4, modelopt_mxfp8
                └── 35+ 种方法
```

### 3.2 核心组件

#### 3.2.1 工厂函数 `build_quant_config()`

**位置**: `vllm_omni/quantization/factory.py`

```python
def build_quant_config(
    spec: str | dict[str, Any] | QuantizationConfig | None,
    **kwargs: Any,
) -> QuantizationConfig | None:
    """从灵活的规范构建量化配置。

    支持的输入格式：
    - None/"none" → 返回 None
    - 字符串 "fp8" → 通过 _build_single() 构建
    - 字典 {"method": "fp8", ...} → 合并 kwargs 后构建
    - 按组件字典 {"transformer": "fp8", "vae": None} → ComponentQuantizationConfig
    - QuantizationConfig 对象 → 直接返回（透传）
    """
```

#### 3.2.2 按组件量化配置 `ComponentQuantizationConfig`

**位置**: `vllm_omni/quantization/component_config.py`

用于多阶段模型（如 DiT + VAE），通过最长前缀匹配路由量化方法：

```python
# 示例：transformer 使用 FP8，VAE 不量化
config = build_quant_config({
    "transformer": {"method": "fp8"},
    "vae": None,
})

# 路由逻辑
config.resolve("transformer.blocks.0.attn.to_q")  → fp8_config
config.resolve("vae.encoder.conv_in")             → None
```

#### 3.2.3 量化配置基类

所有量化配置继承自 vLLM 的 `QuantizationConfig`：

```python
class QuantizationConfig:
    def get_name(self) -> QuantizationMethods: ...
    def get_supported_act_dtypes(self) -> list[torch.dtype]: ...
    def get_min_capability(cls) -> int: ...
    def get_config_filenames(cls) -> list[str]: ...
    def from_config(cls, config: dict[str, Any]) -> "QuantizationConfig": ...
    def get_quant_method(self, layer, prefix) -> QuantizeMethodBase | None: ...
```

### 3.3 支持的量化方法

#### 扩散模型（DiT）

| 方法 | 说明 | 测试模型 | 最低硬件 |
|------|------|---------|---------|
| FP8 | FP8 W8A8，动态或静态 | Z-Image, Qwen-Image, Flux | SM 89 (Ada) |
| Int8 | Int8 W8A8 | Z-Image, Qwen-Image | SM 89 / Ascend NPU |
| GGUF | GGUF 格式，dequant+GEMM | Z-Image, Flux | SM 60 |
| AutoRound | W4A16（预量化） | Flux | SM 80 (Ampere) |

#### 多阶段 Omni 模型（预量化检查点）

| 方法 | 说明 | 测试模型 | 最低硬件 |
|------|------|---------|---------|
| ModelOpt FP8 | NVIDIA ModelOpt 预量化 FP8 | Qwen3-Omni (thinker) | SM 89 (Ada/Hopper) |
| ModelOpt NVFP4 | NVIDIA ModelOpt 预量化 NVFP4 | Qwen3-Omni（实验性） | SM 100 (Blackwell) |

**重要**：`modelopt_fp4` 和 `modelopt_mxfp8` 已在 `SUPPORTED_QUANTIZATION_METHODS` 中注册，但仅支持预量化检查点加载，**无原生动态量化实现**。

---

## 4. 量化模型加载流程

### 4.1 配置构建

#### Python API

```python
from vllm_omni import Omni
from vllm_omni.quantization import build_quant_config

# 方式 1：字符串
omni = Omni(model="your-model", quantization="fp8")

# 方式 2：字典（带参数）
omni = Omni(
    model="your-model",
    quantization_config={
        "method": "fp8",
        "activation_scheme": "dynamic",
        "ignored_layers": ["img_mlp"],
    },
)

# 方式 3：按组件配置（多阶段模型）
omni = Omni(
    model="your-model",
    quantization_config={
        "transformer": {"method": "fp8"},
        "vae": None,
    },
)
```

#### CLI

```bash
# 在线服务
vllm serve your-model --omni --quantization fp8

# 跳过敏感层
vllm serve your-model --omni --quantization fp8 --ignored-layers "img_mlp"

# JSON 配置
vllm serve your-model --omni --quantization-config '{"method":"gguf","gguf_model":"/path/model.gguf"}'
```

#### Stage Config YAML

```yaml
# vllm_omni/model_executor/stage_configs/hunyuan_image3_moe_dit_2gpu_fp8.yaml
engine_args:
  quantization: "fp8"
```

### 4.2 模型加载入口

量化配置在 Pipeline 初始化时构建并传递给 Transformer：

```python
# 在 Pipeline.__init__ 中
from vllm_omni.quantization import build_quant_config

class YourPipeline(nn.Module):
    def __init__(self, *, od_config: OmniDiffusionConfig, prefix: str = ""):
        super().__init__()

        # 构建量化配置
        quant_config = build_quant_config(od_config.quantization_config)

        # 传递给 Transformer
        self.transformer = YourTransformer2DModel(
            od_config=od_config,
            quant_config=quant_config,
        )

        # 对 Text Encoder 和 VAE 应用 FP8 权重存储（hook 方式）
        if od_config.quantization_config is not None:
            apply_fp8_weight_storage(self.vae)
            apply_fp8_weight_storage(self.text_encoder)
```

### 4.3 权重加载与处理

#### vLLM 并行线性层

DiT 中的 `nn.Linear` 替换为 vLLM 并行线性层，这些层接受 `quant_config`：

```python
from vllm.model_executor.layers.linear import (
    ColumnParallelLinear,
    QKVParallelLinear,
    RowParallelLinear,
)

class YourAttentionBlock(nn.Module):
    def __init__(self, hidden_size, num_heads, quant_config=None, prefix=""):
        # QKV 投影
        self.to_qkv = QKVParallelLinear(
            hidden_size=hidden_size,
            head_size=hidden_size // num_heads,
            total_num_heads=num_heads,
            quant_config=quant_config,  # 量化配置
            prefix=f"{prefix}.to_qkv",  # 用于 ignored_layers 匹配
        )
        # 输出投影
        self.to_out = RowParallelLinear(
            input_size=hidden_size,
            output_size=hidden_size,
            quant_config=quant_config,
            prefix=f"{prefix}.to_out",
        )
```

#### 权重加载流程

1. **创建权重参数**：`create_weights()` 在层上注册量化权重参数
2. **加载权重**：权重加载器从检查点加载数据
3. **后处理**：`process_weights_after_loading()` 处理权重（如量化、转置）
4. **前向计算**：`apply()` 执行量化计算

**Lazy Weight Loading（元设备延迟加载）**：

对于在线量化（从 BF16/FP16 检查点动态量化），使用 `LazyWeightMixin` 在 meta 设备上创建参数，在首次加载时物化：

```python
class LazyWeightMixin:
    uses_meta_device: bool = True

    def create_weights(self, layer, ...):
        # 在 meta 设备上创建参数
        weight = ModelWeightParameter(
            data=torch.empty(..., device="meta", dtype=params_dtype),
            ...
        )
        layer._load_device = torch.get_default_device()  # 保存目标设备
        layer.register_parameter("weight", weight)
```

---

## 5. 平台差异：GPU vs NPU

### 5.1 平台抽象层

**位置**: `vllm_omni/platforms/`

```
platforms/
├── __init__.py          # 平台检测与 current_omni_platform 懒加载
├── interface.py         # OmniPlatform 抽象基类
├── cuda/
│   └── platform.py      # CudaOmniPlatform (继承 vLLM CudaPlatformBase)
├── npu/
│   ├── platform.py      # NPUOmniPlatform (继承 vllm-ascend NPUPlatform)
│   ├── worker/          # NPU 专用 Worker
│   ├── models/          # NPU 专用模型实现
│   ├── stage_configs/   # NPU 专用 Stage Configs
│   └── profiler.py      # NPU 专用 Profiler
├── rocm/
├── xpu/
└── musa/
```

#### 平台检测与激活

```python
# vllm_omni/platforms/__init__.py
def cuda_omni_platform_plugin() -> str | None:
    """检测 CUDA 平台"""
    try:
        import pynvml
        pynvml.nvmlInit()
        if pynvml.nvmlDeviceGetCount() > 0:
            return "vllm_omni.platforms.cuda.platform.CudaOmniPlatform"
    except Exception:
        pass
    return None

def npu_omni_platform_plugin() -> str | None:
    """检测 NPU 平台"""
    try:
        import torch
        if hasattr(torch, "npu") and torch.npu.is_available():
            return "vllm_omni.platforms.npu.platform.NPUOmniPlatform"
    except Exception:
        pass
    return None

# 懒加载当前平台
_current_omni_platform = None

def __getattr__(name: str):
    if name == "current_omni_platform":
        global _current_omni_platform
        if _current_omni_platform is None:
            platform_cls_qualname = resolve_current_omni_platform_cls_qualname()
            _current_omni_platform = resolve_obj_by_qualname(platform_cls_qualname)()
        return _current_omni_platform
```

#### 平台接口方法

```python
# vllm_omni/platforms/interface.py
class OmniPlatform(Platform):
    _omni_enum: OmniPlatformEnum

    def is_cuda(self) -> bool:
        return self._omni_enum == OmniPlatformEnum.CUDA

    def is_npu(self) -> bool:
        return self._omni_enum == OmniPlatformEnum.NPU

    # 设备操作
    @classmethod
    def get_torch_device(cls, local_rank: int | None = None) -> torch.device: ...
    @classmethod
    def get_device_count(cls) -> int: ...
    @classmethod
    def synchronize(cls) -> None: ...
    @classmethod
    def get_free_memory(cls, device: torch.device | None = None) -> int: ...

    # Autocast 上下文
    @classmethod
    def create_autocast_context(cls, *, device_type, dtype, enabled=True): ...

    # Worker 类
    @classmethod
    def get_omni_ar_worker_cls(cls) -> str: ...
    @classmethod
    def get_omni_generation_worker_cls(cls) -> str: ...
```

### 5.2 CUDA 平台实现

```python
# vllm_omni/platforms/cuda/platform.py
class CudaOmniPlatform(OmniPlatform, CudaPlatformBase):
    _omni_enum = OmniPlatformEnum.CUDA

    @classmethod
    def get_torch_device(cls, local_rank=None):
        return torch.device("cuda", local_rank) if local_rank else torch.device("cuda")

    @classmethod
    def get_device_count(cls):
        return torch.cuda.device_count()

    @classmethod
    def get_device_capability(cls, device_id=0):
        major, minor = torch.cuda.get_device_capability(device_id)
        return DeviceCapability(major=major, minor=minor)

    @classmethod
    def get_free_memory(cls, device=None):
        free, _ = torch.cuda.mem_get_info(device)
        return free

    @classmethod
    def supports_torch_inductor(cls):
        return True
```

### 5.3 NPU 平台实现

```python
# vllm_omni/platforms/npu/platform.py
class NPUOmniPlatform(OmniPlatform, NPUPlatform):
    _omni_enum = OmniPlatformEnum.NPU
    dist_backend: str = "hccl"  # NPU 分布式后端

    @classmethod
    def get_torch_device(cls, local_rank=None):
        return torch.device("npu", local_rank) if local_rank else torch.device("npu")

    @classmethod
    def get_device_count(cls):
        return torch.npu.device_count()

    @classmethod
    def get_free_memory(cls, device=None):
        free, _ = torch.npu.mem_get_info(device)
        return free

    @classmethod
    def supports_torch_inductor(cls):
        return False  # NPU 不支持 torch.compile inductor

    @classmethod
    def create_autocast_context(cls, *, device_type, dtype, enabled=True):
        if device_type == "npu":
            return torch.npu.amp.autocast(dtype=dtype)  # NPU 特有
        return super().create_autocast_context(...)
```

### 5.4 量化算子对比

| 操作 | CUDA | NPU |
|------|------|-----|
| **动态量化** | `vllm._custom_ops.scaled_int8_quant()` | `torch_npu.npu_dynamic_quant()` |
| **量化矩阵乘法** | `int8_linear.apply_weights()` (vLLM kernel) | `torch_npu.npu_quant_matmul()` |
| **权重处理** | 标准 vLLM kernel 路径 | 权重转置 `.t().contiguous()`，scale squeeze |
| **依赖库** | `vllm._custom_ops as ops` | `import torch_npu` |
| **FP8 支持** | 原生硬件支持 (SM 89+) | 暂不支持 |
| **FP4 支持** | 原生硬件支持 (SM 100+) | 暂不支持 |

---

## 6. 以 INT8 为例：平台差异化量化实现模式

### 6.1 配置类设计

**位置**: `vllm_omni/quantization/int8_config.py`

```python
class DiffusionInt8Config(QuantizationConfig):
    """INT8 量化配置，支持在线（动态）和离线（检查点）量化。"""

    def __init__(
        self,
        is_checkpoint_int8_serialized: bool = False,
        activation_scheme: str = "dynamic",
        ignored_layers: list[str] | None = None,
    ):
        self.is_checkpoint_int8_serialized = is_checkpoint_int8_serialized
        self.activation_scheme = activation_scheme
        self.ignored_layers = ignored_layers or []

    @classmethod
    def get_name(cls) -> QuantizationMethods:
        return "int8"

    @classmethod
    def get_min_capability(cls) -> int:
        return 80  # A100/H20 已验证
```

### 6.2 Linear Method 实现

#### 基类

```python
class BaseInt8LinearMethod(LinearMethodBase):
    def __init__(self, quant_config: DiffusionInt8Config):
        self.quant_config = quant_config
        self.out_dtype = torch.get_default_dtype()

    def create_weights(self, layer, input_size_per_partition, ...):
        """创建权重参数（INT8 或原始 dtype）"""
        params_dtype = torch.int8 if self.quant_config.is_checkpoint_int8_serialized else params_dtype
        weight = create_weight_parameter(...)
        layer.register_parameter("weight", weight)

        if self.quant_config.is_checkpoint_int8_serialized:
            scale = ChannelQuantScaleParameter(...)
            layer.register_parameter("weight_scale", scale)
```

#### CUDA 离线量化方法

```python
class Int8LinearMethod(BaseInt8LinearMethod):
    def __init__(self, quant_config):
        super().__init__(quant_config)
        # 使用 vLLM 内置 INT8 kernel
        self.int8_linear = init_int8_linear_kernel(
            is_channelwise=False,
            is_static_input_scheme=False,
            input_symmetric=True,
            module_name=self.__class__.__name__,
        )

    def process_weights_after_loading(self, layer):
        self.int8_linear.process_weights_after_loading(layer)

    def apply(self, layer, x, bias=None):
        return self.int8_linear.apply_weights(layer, x, bias)
```

#### NPU 离线量化方法

```python
class NPUInt8LinearMethod(BaseInt8LinearMethod):
    def process_weights_after_loading(self, layer):
        # NPU 需要转置权重
        layer.weight.data = layer.weight.data.t().contiguous()
        layer.weight_scale.data = layer.weight_scale.data.squeeze()

    def apply(self, layer, x, bias=None):
        ori_shape = x.shape
        ori_dtype = x.dtype

        x = x.reshape(-1, ori_shape[-1])
        # NPU 动态量化激活
        quantized_x, pertoken_scale = torch_npu.npu_dynamic_quant(x)

        # NPU 量化矩阵乘法
        output = torch_npu.npu_quant_matmul(
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

#### CUDA 在线量化方法（Lazy Weight Loading）

```python
class Int8OnlineLinearMethod(LazyWeightMixin, Int8LinearMethod):
    """在线版本：从 FP16/BF16 检查点加载，在加载时量化权重。"""

    def process_weights_after_loading(self, layer):
        if getattr(layer, "_already_called_process_weights_after_loading", False):
            return

        # 如果权重在 meta 设备上，物化它
        if layer.weight.device == torch.device("meta"):
            weight = ModelWeightParameter(
                data=torch.empty_like(layer.weight, device=layer._load_device),
                ...
            )
            layer.register_parameter("weight", weight)
            initialize_single_dummy_weight(layer.weight)

        # 使用 CUDA 算子量化权重
        w_q_name, w_s_name, ... = self.int8_linear.layer_param_names
        qweight, weight_scale, _ = ops.scaled_int8_quant(layer.weight, scale=None)

        # 更新层参数
        replace_parameter(layer, w_q_name, torch.nn.Parameter(qweight.t().data, requires_grad=False))
        replace_parameter(layer, w_s_name, torch.nn.Parameter(weight_scale.data, requires_grad=False))

        layer._already_called_process_weights_after_loading = True
```

#### NPU 在线量化方法

```python
class NPUInt8OnlineLinearMethod(LazyWeightMixin, NPUInt8LinearMethod):
    def process_weights_after_loading(self, layer):
        if getattr(layer, "_already_called_process_weights_after_loading", False):
            return

        # 物化 meta 设备权重
        if layer.weight.device == torch.device("meta"):
            ...

        # 使用 NPU 算子量化权重
        weight = layer.weight
        qweight, weight_scale = torch_npu.npu_dynamic_quant(weight)
        qweight = qweight.t().contiguous()

        replace_parameter(layer, "weight", qweight)
        replace_parameter(layer, "weight_scale", weight_scale)
        layer._already_called_process_weights_after_loading = True
```

### 6.3 平台路由机制

```python
class DiffusionInt8Config(QuantizationConfig):
    def get_quant_method(self, layer, prefix):
        if isinstance(layer, LinearBase):
            if is_layer_skipped(prefix, self.ignored_layers, ...):
                return UnquantizedLinearMethod()

            if not self.is_checkpoint_int8_serialized:
                # 在线量化路由
                if current_omni_platform.is_cuda():
                    return Int8OnlineLinearMethod(self)
                elif current_omni_platform.is_npu():
                    return NPUInt8OnlineLinearMethod(self)
                else:
                    raise NotImplementedError("当前平台不支持 int8 在线量化")
            else:
                # 离线量化路由
                if current_omni_platform.is_cuda():
                    return Int8LinearMethod(self)
                elif current_omni_platform.is_npu():
                    return NPUInt8LinearMethod(self)
                else:
                    raise NotImplementedError("当前平台不支持 int8 离线量化")
        return None
```

**在工厂函数中注册**：

```python
# vllm_omni/quantization/factory.py
def _build_int8(**kw: Any) -> QuantizationConfig:
    """延迟导入以避免在模块加载时引入 CUDA/pynvml。"""
    from .int8_config import DiffusionInt8Config
    return DiffusionInt8Config(**kw)

_OVERRIDES = {
    "gguf": _build_gguf,
    "int8": _build_int8,  # 注册在这里
    "inc": _build_inc,
    "auto-round": _build_inc,
}
```

---

## 7. MXFP4 量化开发指南

### 7.1 MXFP4 概述

MXFP4（Microscaling FP4）是一种 4 位浮点量化格式，具有以下特点：

- **数据格式**：每个权重使用 4 位（1 位符号 + 2 位指数 + 1 位尾数）
- **微缩放**：每 32 个元素共享一个 8 位缩放因子（E8M0 格式）
- **硬件支持**：NVIDIA Blackwell 架构（SM 100+）原生支持
- **优势**：相比 FP8 进一步减少 50% 权重内存，适用于大模型推理

**NVFP4 vs MXFP4**：
- **NVFP4**：NVIDIA 专有的 FP4 格式，需要 ModelOpt 工具链预量化
- **MXFP4**：开放标准的微缩放 FP4 格式，OCP（Open Compute Project）标准

### 7.2 现有 FP4 支持现状

在 vLLM-Omni 中，FP4 支持现状如下：

```python
# vllm_omni/model_executor/models/qwen3_omni/qwen3_omni_moe_thinker.py
_PRE_QUANTIZED_METHODS = {"modelopt", "modelopt_fp4", "modelopt_mxfp8"}
```

| 方法 | 类型 | 状态 | 说明 |
|------|------|------|------|
| `modelopt_fp4` | 预量化检查点 | 已注册 | 需要 NVIDIA ModelOpt 预量化 |
| `modelopt_mxfp8` | 预量化检查点 | 已注册 | 需要 NVIDIA ModelOpt 预量化 |
| `mxfp4` | 动态量化 | **未实现** | 需要从头开发 |

**测试覆盖**：
```python
# tests/diffusion/quantization/test_fp8_config.py
for method in ["fp8", "gguf", "awq", "gptq", "bitsandbytes", "modelopt", "modelopt_fp4"]:
    assert method in SUPPORTED_QUANTIZATION_METHODS
```

### 7.3 开发路径选择

开发 MXFP4 量化支持有两条路径：

#### 路径 A：基于 vLLM 上游扩展（推荐）

如果 vLLM 上游已支持或计划支持 MXFP4：
1. 等待/贡献 vLLM 上游的 MXFP4 实现
2. 在 vLLM-Omni 中通过 `_OVERRIDES` 添加扩散模型适配
3. 添加 NPU 平台适配层

#### 路径 B：从零实现（当前场景）

需要完整实现：
1. `DiffusionMXFP4Config` - 量化配置类
2. `MXFP4LinearMethod` - CUDA 线性方法
3. `NPU_MXFP4LinearMethod` - NPU 线性方法
4. 在 `factory.py` 中注册

### 7.4 从零实现 MXFP4 量化

#### 7.4.1 创建配置文件

**新文件**: `vllm_omni/quantization/mxfp4_config.py`

```python
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright contributors to the vLLM project
"""MXFP4 量化配置，用于扩散模型。

MXFP4 (Microscaling FP4) 是一种 4 位浮点量化格式，
每 32 个元素共享一个 8 位缩放因子 (E8M0)。
"""

from collections.abc import Callable
from typing import TYPE_CHECKING, Any, Optional

import torch
from torch.nn import Module
from vllm.logger import init_logger
from vllm.model_executor.layers.linear import (
    LinearBase,
    LinearMethodBase,
    UnquantizedLinearMethod,
)
from vllm.model_executor.layers.quantization import QuantizationMethods
from vllm.model_executor.layers.quantization.base_config import (
    QuantizationConfig,
    QuantizeMethodBase,
)
from vllm.model_executor.layers.quantization.utils.quant_utils import (
    is_layer_skipped,
)
from vllm.model_executor.parameter import (
    ModelWeightParameter,
    ChannelQuantScaleParameter,
)

from vllm_omni.platforms import current_omni_platform

if current_omni_platform.is_npu():
    import torch_npu
else:
    torch_npu = None

if TYPE_CHECKING:
    from vllm.model_executor.models.utils import WeightsMapper

logger = init_logger(__name__)

# MXFP4 block size (OCP standard)
MXFP4_BLOCK_SIZE = 32


class DiffusionMXFP4Config(QuantizationConfig):
    """MXFP4 量化配置，用于扩散模型。

    支持在线（动态）量化和离线（预量化检查点）量化。
    """

    def __init__(
        self,
        is_checkpoint_mxfp4_serialized: bool = False,
        activation_scheme: str = "dynamic",
        ignored_layers: list[str] | None = None,
        weight_block_size: int = MXFP4_BLOCK_SIZE,
    ) -> None:
        super().__init__()

        self.is_checkpoint_mxfp4_serialized = is_checkpoint_mxfp4_serialized
        self.activation_scheme = activation_scheme
        self.ignored_layers = ignored_layers or []
        self.weight_block_size = weight_block_size

    @classmethod
    def get_name(cls) -> QuantizationMethods:
        return "mxfp4"

    @classmethod
    def get_supported_act_dtypes(cls) -> list[torch.dtype]:
        return [torch.bfloat16, torch.float16]

    @classmethod
    def get_min_capability(cls) -> int:
        # MXFP4 需要 Blackwell (SM 100+) 原生支持
        # 对于旧 GPU，可以使用 dequant+GEMM 回退
        return 70  # 允许 Volta 及以上使用软件回退

    @classmethod
    def get_config_filenames(cls) -> list[str]:
        return []

    def apply_vllm_mapper(self, hf_to_vllm_mapper: "WeightsMapper"):
        if self.ignored_layers is not None:
            self.ignored_layers = hf_to_vllm_mapper.apply_list(self.ignored_layers)

    @classmethod
    def from_config(cls, config: dict[str, Any]) -> "DiffusionMXFP4Config":
        quant_method = cls.get_from_keys(config, ["quant_method"])
        is_checkpoint_mxfp4_serialized = "mxfp4" in quant_method.lower()
        activation_scheme = cls.get_from_keys_or(config, ["activation_scheme"], "dynamic")
        ignored_layers = cls.get_from_keys_or(config, ["ignored_layers"], None)
        weight_block_size = cls.get_from_keys_or(config, ["weight_block_size"], MXFP4_BLOCK_SIZE)

        if not ignored_layers:
            ignored_layers = cls.get_from_keys_or(config, ["modules_to_not_convert"], None)

        return cls(
            is_checkpoint_mxfp4_serialized=is_checkpoint_mxfp4_serialized,
            activation_scheme=activation_scheme,
            ignored_layers=ignored_layers,
            weight_block_size=weight_block_size,
        )

    def get_quant_method(
        self,
        layer: torch.nn.Module,
        prefix: str,
    ) -> Optional["QuantizeMethodBase"]:
        if isinstance(layer, LinearBase):
            if is_layer_skipped(
                prefix=prefix,
                ignored_layers=self.ignored_layers,
                fused_mapping=self.packed_modules_mapping,
            ):
                return UnquantizedLinearMethod()

            if not self.is_checkpoint_mxfp4_serialized:
                # 在线量化路由
                if current_omni_platform.is_cuda():
                    return CudaMXFP4OnlineLinearMethod(self)
                elif current_omni_platform.is_npu():
                    return NPUMXFP4OnlineLinearMethod(self)
                else:
                    raise NotImplementedError("当前平台不支持 mxfp4 在线量化")
            else:
                # 离线量化路由
                if current_omni_platform.is_cuda():
                    return CudaMXFP4LinearMethod(self)
                elif current_omni_platform.is_npu():
                    return NPUMXFP4LinearMethod(self)
                else:
                    raise NotImplementedError("当前平台不支持 mxfp4 离线量化")
        return None
```

#### 7.4.2 MXFP4 量化/反量化函数

```python
def quantize_mxfp4(
    weight: torch.Tensor,
    block_size: int = MXFP4_BLOCK_SIZE,
) -> tuple[torch.Tensor, torch.Tensor]:
    """将权重量化为 MXFP4 格式。

    Args:
        weight: 输入权重张量 (BF16/FP16)
        block_size: 缩放块大小 (默认 32)

    Returns:
        qweight: 量化后的 4 位权重 (打包为 int8 张量)
        scale: 每块缩放因子 (E8M0 格式)
    """
    original_shape = weight.shape
    weight = weight.view(-1)

    # 填充到 block_size 的倍数
    num_elements = weight.numel()
    padded_size = ((num_elements + block_size - 1) // block_size) * block_size
    if padded_size > num_elements:
        weight = torch.nn.functional.pad(weight, (0, padded_size - num_elements))

    # 重塑为 (num_blocks, block_size)
    weight = weight.view(-1, block_size)

    # 计算每块的最大绝对值作为缩放因子
    amax = weight.abs().amax(dim=-1, keepdim=True)
    # E8M0 缩放因子：2^exponent，exponent 范围 0-255
    # 使用 log2 计算指数
    scale = torch.clamp(
        torch.ceil(torch.log2(amax + 1e-12)).to(torch.int32),
        min=0,
        max=255,
    )

    # 缩放并量化到 4 位
    # FP4 格式: 1 位符号 + 2 位指数 + 1 位尾数
    # 可表示值: 0, ±0.5, ±1, ±1.5, ±2, ±3, ±4, ±6
    scaled_weight = weight / (2 ** scale.to(weight.dtype))

    # 量化到最近的 FP4 值
    # FP4 查找表
    fp4_values = torch.tensor([0.0, 0.5, 1.0, 1.5, 2.0, 3.0, 4.0, 6.0], device=weight.device)
    abs_scaled = scaled_weight.abs()
    quantized_abs = fp4_values[torch.argmin((abs_scaled.unsqueeze(-1) - fp4_values).abs(), dim=-1)]
    quantized = torch.sign(scaled_weight) * quantized_abs

    # 打包 4 位值到 int8（每个字节存储 2 个 4 位值）
    # 将 FP4 值编码为 4 位索引
    fp4_to_index = {v: i for i, v in enumerate(fp4_values.tolist())}
    abs_to_index = torch.zeros(7, dtype=torch.int32, device=weight.device)
    for v, i in fp4_to_index.items():
        if v > 0:
            abs_to_index[int(v * 2) - 1] = i

    sign_bit = (scaled_weight < 0).to(torch.int32)
    abs_index = torch.zeros_like(scaled_weight, dtype=torch.int32)
    for i, v in enumerate(fp4_values.tolist()):
        if v > 0:
            abs_index[(quantized_abs == v)] = fp4_to_index[v]

    # 编码: 3 位绝对值索引 + 1 位符号
    packed_4bit = (abs_index << 1) | sign_bit

    # 打包为 int8
    packed_4bit = packed_4bit.view(-1)
    qweight = (packed_4bit[0::2] << 4 | packed_4bit[1::2]).to(torch.int8)

    # 恢复原始形状信息
    qweight = qweight.view(-1)
    scale = scale.squeeze(-1)

    return qweight, scale.to(torch.float32)


def dequantize_mxfp4(
    qweight: torch.Tensor,
    scale: torch.Tensor,
    block_size: int = MXFP4_BLOCK_SIZE,
    dtype: torch.dtype = torch.bfloat16,
) -> torch.Tensor:
    """从 MXFP4 格式反量化权重。

    Args:
        qweight: 量化后的 4 位权重 (打包为 int8)
        scale: 每块缩放因子
        block_size: 缩放块大小
        dtype: 输出数据类型

    Returns:
        weight: 反量化后的权重张量
    """
    # 解包 int8 为 4 位值
    packed = qweight.to(torch.int32)
    high_nibbles = (packed >> 4) & 0xF
    low_nibbles = packed & 0xF

    # 交错排列
    unpacked = torch.empty(qweight.numel() * 2, dtype=torch.int32, device=qweight.device)
    unpacked[0::2] = high_nibbles
    unpacked[1::2] = low_nibbles

    # 解码 4 位值为 FP4
    # 3 位绝对值索引 + 1 位符号
    abs_index = (unpacked >> 1) & 0x7
    sign_bit = unpacked & 0x1

    fp4_values = torch.tensor([0.0, 0.5, 1.0, 1.5, 2.0, 3.0, 4.0, 6.0], device=qweight.device)
    abs_values = fp4_values[abs_index]
    signs = torch.where(sign_bit == 1, -1.0, 1.0)
    fp4_values_expanded = abs_values * signs

    # 应用缩放因子
    num_blocks = scale.numel()
    scale_expanded = scale.repeat_interleave(block_size)
    scale_expanded = scale_expanded[:fp4_values_expanded.numel()]

    weight = fp4_values_expanded * (2 ** scale_expanded.to(dtype))

    return weight.to(dtype)
```

### 7.5 GPU 端 MXFP4 实现

#### 7.5.1 CUDA 离线量化方法

```python
class CudaMXFP4LinearMethod(LinearMethodBase):
    """CUDA MXFP4 线性方法，支持加载预量化检查点。"""

    def __init__(self, quant_config: DiffusionMXFP4Config):
        self.quant_config = quant_config

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
        output_size_per_partition = sum(output_partition_sizes)
        weight_loader = extra_weight_attrs.get("weight_loader")

        layer.logical_widths = output_partition_sizes
        layer.input_size_per_partition = input_size_per_partition
        layer.output_size_per_partition = output_size_per_partition
        layer.orig_dtype = params_dtype

        # MXFP4 权重：每 2 个 4 位值打包为 1 个 int8
        weight = ModelWeightParameter(
            data=torch.empty(
                output_size_per_partition,
                input_size_per_partition // 2,  # 打包后大小减半
                dtype=torch.int8,
            ),
            input_dim=1,
            output_dim=0,
            weight_loader=weight_loader,
        )
        layer.register_parameter("weight", weight)

        # MXFP4 缩放因子：每 block_size 个元素一个缩放因子
        num_scales = input_size_per_partition // self.quant_config.weight_block_size
        scale = ChannelQuantScaleParameter(
            data=torch.empty(output_size_per_partition, num_scales, dtype=torch.float32),
            output_dim=0,
            weight_loader=weight_loader,
        )
        layer.register_parameter("weight_scale", scale)

    def process_weights_after_loading(self, layer: Module) -> None:
        # CUDA 端权重处理（如果需要）
        pass

    def apply(
        self,
        layer: torch.nn.Module,
        x: torch.Tensor,
        bias: torch.Tensor | None = None,
    ) -> torch.Tensor:
        """使用 MXFP4 权重执行线性计算。

        对于 Blackwell GPU (SM 100+)，使用原生 FP4 GEMM。
        对于旧 GPU，使用 dequant+GEMM 回退。
        """
        from vllm_omni.platforms.cuda.platform import CudaOmniPlatform

        capability = CudaOmniPlatform.get_device_capability()
        has_native_fp4 = capability is not None and capability.major >= 10  # Blackwell

        if has_native_fp4:
            # TODO: 使用原生 FP4 GEMM kernel
            # 例如: torch._C._cuda_fp4_gemm(x, layer.weight, layer.weight_scale)
            raise NotImplementedError("Native FP4 GEMM not yet implemented")
        else:
            # 软件回退：反量化 + GEMM
            weight = dequantize_mxfp4(
                layer.weight,
                layer.weight_scale,
                block_size=self.quant_config.weight_block_size,
                dtype=x.dtype,
            )
            output = x @ weight.T
            if bias is not None:
                output = output + bias
            return output
```

#### 7.5.2 CUDA 在线量化方法

```python
class LazyWeightMixin:
    """Mixin for lazy weight loading with meta device."""
    uses_meta_device: bool = True

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
        output_size_per_partition = sum(output_partition_sizes)
        weight_loader = extra_weight_attrs.get("weight_loader")

        layer.logical_widths = output_partition_sizes
        layer.input_size_per_partition = input_size_per_partition
        layer.output_size_per_partition = output_size_per_partition
        layer.orig_dtype = params_dtype
        layer.weight_block_size = None

        def patched_weight_loader(param, loaded_weight, *args, **kwargs):
            if not hasattr(layer, "_loaded_numel"):
                layer._loaded_numel = 0

                # 首次加载时物化参数
                from vllm.model_executor.layers.quantization.fp8 import (
                    CopyNumelCounter,
                    _copy_missing_attrs,
                )

                weight = ModelWeightParameter(
                    data=torch.empty_like(layer.weight, device=layer._load_device),
                    input_dim=1,
                    output_dim=0,
                    weight_loader=patched_weight_loader,
                )
                _copy_missing_attrs(layer.weight, weight)
                layer.register_parameter("weight", weight)
                del layer._load_device

            param = layer.weight

            copy_numel_counter = CopyNumelCounter()
            with copy_numel_counter:
                res = weight_loader(param, loaded_weight, *args, **kwargs)
            layer._loaded_numel += copy_numel_counter.copied_numel

            if layer._loaded_numel == layer.weight.numel():
                self.process_weights_after_loading(layer)
                layer._already_called_process_weights_after_loading = True

            return res

        # 在 meta 设备上创建参数
        weight = ModelWeightParameter(
            data=torch.empty(
                output_size_per_partition,
                input_size_per_partition,
                device="meta",
                dtype=params_dtype,
            ),
            input_dim=1,
            output_dim=0,
            weight_loader=patched_weight_loader,
        )
        layer._load_device = torch.get_default_device()
        layer.register_parameter("weight", weight)


class CudaMXFP4OnlineLinearMethod(LazyWeightMixin, CudaMXFP4LinearMethod):
    """CUDA MXFP4 在线量化方法，从 BF16/FP16 检查点动态量化。"""

    def process_weights_after_loading(self, layer: Module) -> None:
        if getattr(layer, "_already_called_process_weights_after_loading", False):
            return

        if layer.weight.device == torch.device("meta"):
            from vllm.model_executor.layers.quantization.fp8 import _copy_missing_attrs
            from vllm.model_executor.model_loader.weight_utils import initialize_single_dummy_weight

            weight = ModelWeightParameter(
                data=torch.empty_like(layer.weight, device=layer._load_device),
                input_dim=1,
                output_dim=0,
                weight_loader=layer.weight.weight_loader,
            )
            _copy_missing_attrs(layer.weight, weight)
            layer.register_parameter("weight", weight)
            initialize_single_dummy_weight(layer.weight)

        # 量化权重为 MXFP4
        qweight, weight_scale = quantize_mxfp4(
            layer.weight,
            block_size=self.quant_config.weight_block_size,
        )

        # 更新层参数
        from vllm.model_executor.utils import replace_parameter

        replace_parameter(layer, "weight", torch.nn.Parameter(qweight, requires_grad=False))
        replace_parameter(layer, "weight_scale", torch.nn.Parameter(weight_scale, requires_grad=False))

        layer._already_called_process_weights_after_loading = True
```

### 7.6 NPU 端 MXFP4 实现

#### 7.6.1 NPU 离线量化方法

```python
class NPUMXFP4LinearMethod(LinearMethodBase):
    """NPU MXFP4 线性方法。

    NPU 使用 torch_npu 提供的量化算子。
    """

    def __init__(self, quant_config: DiffusionMXFP4Config):
        self.quant_config = quant_config

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
        output_size_per_partition = sum(output_partition_sizes)
        weight_loader = extra_weight_attrs.get("weight_loader")

        layer.logical_widths = output_partition_sizes
        layer.input_size_per_partition = input_size_per_partition
        layer.output_size_per_partition = output_size_per_partition
        layer.orig_dtype = params_dtype

        # NPU 权重格式（可能需要转置）
        weight = ModelWeightParameter(
            data=torch.empty(
                output_size_per_partition,
                input_size_per_partition // 2,
                dtype=torch.int8,
            ),
            input_dim=1,
            output_dim=0,
            weight_loader=weight_loader,
        )
        layer.register_parameter("weight", weight)

        num_scales = input_size_per_partition // self.quant_config.weight_block_size
        scale = ChannelQuantScaleParameter(
            data=torch.empty(output_size_per_partition, num_scales, dtype=torch.float32),
            output_dim=0,
            weight_loader=weight_loader,
        )
        layer.register_parameter("weight_scale", scale)

    def process_weights_after_loading(self, layer: Module) -> None:
        # NPU 需要转置权重
        layer.weight.data = layer.weight.data.t().contiguous()
        layer.weight_scale.data = layer.weight_scale.data.squeeze()

    def apply(
        self,
        layer: torch.nn.Module,
        x: torch.Tensor,
        bias: torch.Tensor | None = None,
    ) -> torch.Tensor:
        """NPU MXFP4 线性计算。

        使用 torch_npu 提供的量化算子或软件回退。
        """
        ori_shape = x.shape
        ori_dtype = x.dtype

        x = x.reshape(-1, ori_shape[-1])

        # 检查 NPU 是否支持 FP4 算子
        # 注意：当前 torch_npu 可能不支持原生 FP4，需要软件回退
        if hasattr(torch_npu, "npu_fp4_matmul"):
            # TODO: 使用原生 FP4 GEMM（如果可用）
            output = torch_npu.npu_fp4_matmul(
                x,
                layer.weight,
                layer.weight_scale,
                bias=bias,
                output_dtype=ori_dtype,
            )
        else:
            # 软件回退：反量化 + GEMM
            weight = dequantize_mxfp4(
                layer.weight,
                layer.weight_scale,
                block_size=self.quant_config.weight_block_size,
                dtype=x.dtype,
            )
            # NPU 矩阵乘法
            output = torch_npu.npu_quant_matmul(
                x,
                weight,
                torch.ones(1, device=x.device),  # 无缩放
                bias=bias,
                output_dtype=ori_dtype,
            )

        output = output.reshape(*ori_shape[:-1], -1)
        return output
```

#### 7.6.2 NPU 在线量化方法

```python
class NPUMXFP4OnlineLinearMethod(LazyWeightMixin, NPUMXFP4LinearMethod):
    """NPU MXFP4 在线量化方法。"""

    def process_weights_after_loading(self, layer: Module) -> None:
        if getattr(layer, "_already_called_process_weights_after_loading", False):
            return

        if layer.weight.device == torch.device("meta"):
            from vllm.model_executor.layers.quantization.fp8 import _copy_missing_attrs
            from vllm.model_executor.model_loader.weight_utils import initialize_single_dummy_weight

            weight = ModelWeightParameter(
                data=torch.empty_like(layer.weight, device=layer._load_device),
                input_dim=1,
                output_dim=0,
                weight_loader=layer.weight.weight_loader,
            )
            _copy_missing_attrs(layer.weight, weight)
            layer.register_parameter("weight", weight)
            initialize_single_dummy_weight(layer.weight)

        # 使用 NPU 算子量化权重（或软件回退）
        if hasattr(torch_npu, "npu_quantize_mxfp4"):
            qweight, weight_scale = torch_npu.npu_quantize_mxfp4(
                layer.weight,
                block_size=self.quant_config.weight_block_size,
            )
        else:
            # 软件回退
            qweight, weight_scale = quantize_mxfp4(
                layer.weight,
                block_size=self.quant_config.weight_block_size,
            )

        qweight = qweight.t().contiguous()

        from vllm.model_executor.utils import replace_parameter

        replace_parameter(layer, "weight", qweight)
        replace_parameter(layer, "weight_scale", weight_scale)

        layer._already_called_process_weights_after_loading = True
```

### 7.7 注册与集成

#### 7.7.1 在工厂函数中注册

**修改文件**: `vllm_omni/quantization/factory.py`

```python
def _build_mxfp4(**kw: Any) -> QuantizationConfig:
    """延迟导入 MXFP4 配置。"""
    from .mxfp4_config import DiffusionMXFP4Config
    return DiffusionMXFP4Config(**kw)


_OVERRIDES = {
    "gguf": _build_gguf,
    "int8": _build_int8,
    "inc": _build_inc,
    "auto-round": _build_inc,
    "mxfp4": _build_mxfp4,  # 新增
}
```

#### 7.7.2 更新 `__init__.py`

**修改文件**: `vllm_omni/quantization/__init__.py`

```python
from .component_config import ComponentQuantizationConfig
from .factory import SUPPORTED_QUANTIZATION_METHODS, build_quant_config

__all__ = [
    "build_quant_config",
    "ComponentQuantizationConfig",
    "SUPPORTED_QUANTIZATION_METHODS",
]
```

#### 7.7.3 在 Transformer 中集成

```python
# 在 Transformer 构造函数中接受 quant_config
class YourTransformer2DModel(nn.Module):
    def __init__(
        self,
        *,
        od_config: OmniDiffusionConfig,
        quant_config: QuantizationConfig | None = None,
        # ... other params
    ):
        self.quant_config = quant_config

        # 传递给子模块
        self.blocks = nn.ModuleList([
            YourTransformerBlock(
                hidden_size=hidden_size,
                quant_config=quant_config,
                prefix=f"blocks.{i}",
            )
            for i in range(num_layers)
        ])

# 在 Attention Block 中使用
class YourTransformerBlock(nn.Module):
    def __init__(self, hidden_size, num_heads, quant_config=None, prefix=""):
        self.to_qkv = QKVParallelLinear(
            hidden_size=hidden_size,
            head_size=hidden_size // num_heads,
            total_num_heads=num_heads,
            quant_config=quant_config,
            prefix=f"{prefix}.attn.to_qkv",
        )
        self.to_out = RowParallelLinear(
            input_size=hidden_size,
            output_size=hidden_size,
            quant_config=quant_config,
            prefix=f"{prefix}.attn.to_out",
        )
```

#### 7.7.4 Pipeline 集成

```python
class YourPipeline(nn.Module):
    def __init__(self, *, od_config: OmniDiffusionConfig, prefix: str = ""):
        super().__init__()

        # 构建量化配置
        quant_config = build_quant_config(od_config.quantization_config)

        # 传递给 Transformer
        self.transformer = YourTransformer2DModel(
            od_config=od_config,
            quant_config=quant_config,
        )

        # 可选：对 VAE 和 Text Encoder 应用量化
        if od_config.quantization_config is not None:
            apply_fp8_weight_storage(self.vae)  # 或自定义 mxfp4 存储
```

---

## 8. 测试与验证

### 8.1 单元测试

**新文件**: `tests/diffusion/quantization/test_mxfp4_config.py`

```python
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright contributors to the vLLM project
"""MXFP4 量化配置测试。"""

import pytest
import torch

pytestmark = [pytest.mark.core_model, pytest.mark.diffusion, pytest.mark.cpu]


def test_build_quant_config_mxfp4():
    """测试构建 MXFP4 配置。"""
    from vllm_omni.quantization import build_quant_config

    config = build_quant_config("mxfp4")
    assert config is not None
    assert config.get_name() == "mxfp4"
    assert config.activation_scheme == "dynamic"
    assert config.weight_block_size == 32


def test_build_quant_config_mxfp4_dict():
    """测试从字典构建 MXFP4 配置。"""
    from vllm_omni.quantization import build_quant_config

    config = build_quant_config({
        "method": "mxfp4",
        "activation_scheme": "dynamic",
        "ignored_layers": ["img_mlp"],
        "weight_block_size": 32,
    })
    assert config is not None
    assert config.get_name() == "mxfp4"
    assert config.ignored_layers == ["img_mlp"]


def test_mxfp4_quantize_dequantize():
    """测试 MXFP4 量化/反量化循环。"""
    from vllm_omni.quantization.mxfp4_config import (
        dequantize_mxfp4,
        quantize_mxfp4,
    )

    # 创建测试权重
    weight = torch.randn(64, 128, dtype=torch.bfloat16)

    # 量化
    qweight, scale = quantize_mxfp4(weight, block_size=32)

    # 反量化
    dequant_weight = dequantize_mxfp4(qweight, scale, block_size=32, dtype=torch.bfloat16)

    # 检查形状
    assert dequant_weight.shape == weight.shape
    assert dequant_weight.dtype == torch.bfloat16

    # 检查量化误差（MXFP4 精度较低，容忍度较大）
    relative_error = (dequant_weight - weight).abs() / (weight.abs() + 1e-6)
    assert relative_error.mean() < 0.2  # 平均误差 < 20%


def test_mxfp4_per_component():
    """测试按组件 MXFP4 配置。"""
    from vllm_omni.quantization import ComponentQuantizationConfig, build_quant_config

    config = build_quant_config({
        "transformer": {"method": "mxfp4"},
        "vae": None,
    })
    assert isinstance(config, ComponentQuantizationConfig)
    assert config.component_configs["transformer"].get_name() == "mxfp4"
    assert config.component_configs["vae"] is None


def test_supported_methods_includes_mxfp4():
    """测试 MXFP4 在支持的方法列表中。"""
    from vllm_omni.quantization import SUPPORTED_QUANTIZATION_METHODS

    assert "mxfp4" in SUPPORTED_QUANTIZATION_METHODS
```

### 8.2 质量门控测试

**修改文件**: `tests/diffusion/quantization/test_quantization_quality.py`

```python
# 在 QUALITY_CONFIGS 中添加
QualityTestConfig(
    id="mxfp4_z_image",
    model="Tongyi-MAI/Z-Image-Turbo",
    quantization="mxfp4",
    task="t2i",
    prompt="a cup of coffee on a wooden table, morning light",
    max_lpips=0.15,  # MXFP4 精度较低，阈值放宽
    num_inference_steps=20,
),
```

**运行测试**：
```bash
# 安装依赖
pip install lpips

# 运行所有质量测试
pytest tests/diffusion/quantization/test_quantization_quality.py -v -m ""

# 仅运行 MXFP4 测试
pytest tests/diffusion/quantization/test_quantization_quality.py -v -m "" -k "mxfp4"
```

### 8.3 E2E 测试

**新文件**: `tests/e2e/offline_inference/test_quantization_mxfp4.py`

```python
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright contributors to the vLLM project
"""MXFP4 量化 E2E 测试。"""

import pytest

pytestmark = [pytest.mark.advanced_model, pytest.mark.diffusion]


@pytest.mark.parametrize("model", ["Tongyi-MAI/Z-Image-Turbo"])
def test_mxfp4_offline_inference(model):
    """测试 MXFP4 离线推理。"""
    from vllm_omni import Omni
    from vllm_omni.inputs.data import OmniDiffusionSamplingParams

    omni = Omni(model=model, quantization="mxfp4")

    outputs = omni.generate(
        "A cat sitting on a windowsill",
        OmniDiffusionSamplingParams(num_inference_steps=20, seed=42),
    )

    assert len(outputs) > 0
    assert outputs[0].outputs is not None
```

---

## 9. 调试与常见问题

### 9.1 常见问题

| 问题 | 原因 | 解决方案 |
|------|------|---------|
| `Unknown quantization method: 'mxfp4'` | 未在 `_OVERRIDES` 中注册 | 在 `factory.py` 中添加 `"mxfp4": _build_mxfp4` |
| `NotImplementedError: Native FP4 GEMM not yet implemented` | 旧 GPU 无原生 FP4 支持 | 使用 dequant+GEMM 回退（已实现） |
| 输出质量严重下降 | MXFP4 精度较低 | 使用 `ignored_layers` 跳过敏感层 |
| NPU 上 `torch_npu` 不可用 | 未安装 torch_npu | 确保 NPU 环境正确安装 |
| 权重加载 OOM | 峰值内存仍为 BF16 | FP8/MXFP4 权重存储在 `from_pretrained()` 之后应用 |

### 9.2 调试技巧

```python
# 启用详细日志
import logging
logging.getLogger("vllm_omni").setLevel(logging.DEBUG)

# 检查平台检测
from vllm_omni.platforms import current_omni_platform
print(f"Platform: {current_omni_platform._omni_enum}")
print(f"Is CUDA: {current_omni_platform.is_cuda()}")
print(f"Is NPU: {current_omni_platform.is_npu()}")

# 检查量化配置
from vllm_omni.quantization import build_quant_config
config = build_quant_config("mxfp4")
print(f"Quant method: {config.get_name()}")
print(f"Ignored layers: {config.ignored_layers}")
```

### 9.3 敏感层处理

MXFP4 精度较低，建议对以下层禁用量化：

```python
omni = Omni(
    model="your-model",
    quantization_config={
        "method": "mxfp4",
        "ignored_layers": ["img_mlp", "proj_out", "lm_head", "mlp.gate"],
    },
)
```

---

## 10. 关键文件索引

### 量化核心文件

| 文件路径 | 说明 |
|---------|------|
| `vllm_omni/quantization/__init__.py` | 统一量化入口 |
| `vllm_omni/quantization/factory.py` | 配置工厂，`build_quant_config()` |
| `vllm_omni/quantization/component_config.py` | 按组件量化路由 |
| `vllm_omni/quantization/int8_config.py` | INT8 量化实现（参考模板） |
| `vllm_omni/quantization/gguf_config.py` | GGUF 量化实现 |
| `vllm_omni/quantization/mxfp4_config.py` | **MXFP4 量化实现（需新建）** |

### 平台相关文件

| 文件路径 | 说明 |
|---------|------|
| `vllm_omni/platforms/__init__.py` | 平台检测与 `current_omni_platform` |
| `vllm_omni/platforms/interface.py` | `OmniPlatform` 抽象基类 |
| `vllm_omni/platforms/cuda/platform.py` | CUDA 平台实现 |
| `vllm_omni/platforms/npu/platform.py` | NPU 平台实现 |

### 测试文件

| 文件路径 | 说明 |
|---------|------|
| `tests/diffusion/quantization/test_fp8_config.py` | FP8 配置测试（参考模板） |
| `tests/diffusion/quantization/test_int8_config.py` | INT8 配置测试 |
| `tests/diffusion/quantization/test_quantization_quality.py` | LPIPS 质量门控测试 |
| `tests/diffusion/quantization/test_mxfp4_config.py` | **MXFP4 配置测试（需新建）** |

### 文档文件

| 文件路径 | 说明 |
|---------|------|
| `docs/contributing/model/adding_quantization_model.md` | 添加量化模型指南 |
| `docs/user_guide/diffusion/quantization/overview.md` | 量化概述 |
| `docs/user_guide/diffusion/quantization/fp8.md` | FP8 用户指南 |
| `docs/user_guide/diffusion/quantization/int8.md` | INT8 用户指南 |

### 安装相关文件

| 文件路径 | 说明 |
|---------|------|
| `setup.py` | 安装脚本，平台检测 |
| `pyproject.toml` | 项目配置 |
| `requirements/common.txt` | 通用依赖 |
| `requirements/cuda.txt` | CUDA 依赖 |
| `requirements/npu.txt` | NPU 依赖 |

---

## 附录：开发检查清单

开发 MXFP4 量化支持时，请确保完成以下步骤：

- [ ] 创建 `vllm_omni/quantization/mxfp4_config.py`
  - [ ] `DiffusionMXFP4Config` 配置类
  - [ ] `quantize_mxfp4()` 和 `dequantize_mxfp4()` 函数
  - [ ] `CudaMXFP4LinearMethod`（CUDA 离线）
  - [ ] `CudaMXFP4OnlineLinearMethod`（CUDA 在线）
  - [ ] `NPUMXFP4LinearMethod`（NPU 离线）
  - [ ] `NPUMXFP4OnlineLinearMethod`（NPU 在线）

- [ ] 在 `factory.py` 中注册
  - [ ] 添加 `_build_mxfp4()` 函数
  - [ ] 在 `_OVERRIDES` 字典中添加 `"mxfp4": _build_mxfp4`

- [ ] 编写测试
  - [ ] 单元测试 `test_mxfp4_config.py`
  - [ ] 质量门控测试配置
  - [ ] E2E 测试（可选）

- [ ] 更新文档
  - [ ] 在 `docs/user_guide/diffusion/quantization/overview.md` 中添加 MXFP4 条目
  - [ ] 创建 `docs/user_guide/diffusion/quantization/mxfp4.md` 用户指南
  - [ ] 更新支持模型表格

- [ ] 代码规范
  - [ ] 运行 `pre-commit run --all-files`
  - [ ] 确保所有提交包含 `Signed-off-by`

- [ ] 平台兼容性验证
  - [ ] CUDA 平台测试（SM 89+ 和 SM 100+）
  - [ ] NPU 平台测试（Atlas A2/A3）
  - [ ] 软件回退路径验证
