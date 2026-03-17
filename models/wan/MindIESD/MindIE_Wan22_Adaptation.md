# MindIE Wan 2.2 昇腾适配项目改动详解

本文档详细说明华为昇腾（MindIE）对 Wan 2.2 视频生成模型的适配改动。

---

## 概述

MindIE 是华为昇腾的推理加速框架，该项目将 Wan 2.2 从 CUDA/GPU 迁移到 NPU（昇腾处理器）。主要改动包括：

1. **NPU 基础适配** - 从 CUDA 迁移到 NPU
2. **分布式并行** - 新增 TP/SP/CFG 并行策略
3. **算子优化** - 使用华为自研高性能算子
4. **量化支持** - 支持 W4A4 量化推理
5. **稀疏 Attention** - Rainfusion 稀疏注意力
6. **模型裁剪** - 移除 S2V/Animate 等模块

---

## 一、完整文件改动清单

| 文件 | 改动类型 | 说明 |
|------|----------|------|
| `generate.py` | 修改 | NPU初始化、分布式配置 |
| `wan/__init__.py` | 修改 | 移除S2V/Animate导入 |
| `wan/configs/__init__.py` | 修改 | 配置调整、新增OPTIMAL_PARALLEL |
| `wan/text2video.py` | 修改 | NPU适配、VAE并行、量化支持 |
| `wan/image2video.py` | 修改 | NPU适配、VAE并行、量化支持 |
| `wan/textimage2video.py` | 修改 | NPU适配、VAE并行 |
| `wan/modules/model.py` | 修改 | RoPE/LayerNorm/Attention优化 |
| `wan/modules/attention.py` | 修改 | 导入Rainfusion |
| `wan/modules/t5.py` | 修改 | 导入torch_npu |
| `wan/modules/vae2_1.py` | 修改 | CausalConv3d padding调整 |
| `wan/modules/vae2_2.py` | 修改 | CausalConv3d padding调整 |
| `wan/distributed/util.py` | 修改 | 新增并行组生成函数 |
| `wan/distributed/fsdp.py` | 修改 | 移除use_lora参数 |
| `wan/distributed/sequence_parallel.py` | 修改 | RoPE优化 |
| `wan/utils/fm_solvers_unipc.py` | 修改 | 矩阵求解兼容性 |
| `wan/utils/utils.py` | 修改 | 移除merge_video_audio |
| `wan/distributed/comm.py` | **新增** | NPU分布式通信原语 |
| `wan/distributed/parallel_mgr.py` | **新增** | 分布式并行管理器 |
| `wan/distributed/tp_applicator.py` | **新增** | 张量并行应用器 |
| `wan/distributed/group_coordinator.py` | **新增** | 进程组协调器 |
| `wan/utils/rainfusion.py` | **新增** | 稀疏注意力机制 |
| `wan/utils/rainfusion_blockwise.py` | **新增** | 分块稀疏注意力 |
| `wan/vae_patch_parallel.py` | **新增** | VAE空间并行 |
| `wan/modules/attn_layer.py` | **新增** | Attention层封装 |
| `quant_wan22.py` | **新增** | 量化推理脚本 |

### 1.1 model.py - DiT 主干网络

**文件**: `wan/modules/model.py`

#### 1.1.1 导入部分改动

```python
# 新增导入
import logging
import os
import warnings
import torch_npu                          # NPU 基础库
from mindiesd import rotary_position_embedding, attention_forward  # 华为自研算子

# Fast LayerNorm (可选)
FAST_LAYERNORM = int(os.getenv('FAST_LAYERNORM', 0))
if FAST_LAYERNORM:
    from mindiesd import fast_layernorm

# Rainfusion 稀疏注意力
from wan.utils.rainfusion import Rainfusion
from wan.utils.rainfusion_blockwise import Rainfusion_blockwise
```

#### 1.1.2 RoPE (旋转位置编码) 改动

```python
# 改动1: float64 -> float32
- position = position.type(torch.float64)
+ position = position.type(torch.float32)

# 改动2: npu 设备支持
- @torch.amp.autocast('cuda', enabled=False)
+ @torch.amp.autocast('npu', enabled=False)

# 改动3: 使用华为自研算子替代手写实现
- def rope_apply(x, grid_sizes, freqs):
-     # 手写实现 (Python循环)
-     for i, (f, h, w) in enumerate(grid_sizes.tolist()):
-         x_i = torch.view_as_complex(...)
-         ...

+ def rope_apply(x, grid_sizes, freqs_list):
+     cos, sin = freqs_list[0]
+     # 使用华为融合算子
+     return rotary_position_embedding(x, cos, sin, rotated_mode="rotated_interleaved", fused=True)
```

**改动说明**:
- 原来的手写实现使用 Python 循环，在 NPU 上效率较低
- 使用华为自研的 `rotary_position_embedding` 算子，融合了计算图，性能更高

#### 1.1.3 LayerNorm 改动

```python
# RMSNorm: 使用 NPU 融合算子
- def forward(self, x):
-     return self._norm(x.float()).type_as(x) * self.weight
+ def forward(self, x):
+     return torch_npu.npu_rms_norm(x, self.weight, epsilon=self.eps)[0]

# LayerNorm: 使用 NPU 算子
- def forward(self, x):
-     return super().forward(x.float()).type_as(x)
+ def forward(self, x):
+     return torch.nn.functional.layer_norm(
+         x, normalized_shape=[self.dim], weight=self.weight, bias=self.bias, eps=self.eps
+     )
```

**改动说明**:
- 使用 NPU 融合算子 `npu_rms_norm` 和 `layer_norm`
- 融合算子可减少 kernel 启动开销，提升性能

#### 1.1.4 Attention 改动 - 核心

```python
# 新增: 多注意力后端选择
class WanSelfAttention(nn.Module):
    def __init__(self, ...):
        ...
        # 新增: 子序列并行头配置
        self.use_sub_head = int(os.getenv('USE_SUB_HEAD', 0))
    
    def attention(self, q, k, v, **kwargs):
        """统一注意力接口，支持多种后端"""
        if self.use_sub_head:
            # 分头处理 (用于序列并行)
            query_layer_list = q.split(self.use_sub_head, dim=2)
            ...
        else:
            return self._attention_op(q, k, v, **kwargs)
    
    def _attention_op(self, q, k, v, ...):
        if torch.npu.is_available():
            # 优先级: Rainfusion > Laser > 融合Attention > FlashAttention
            if rainfusion_config is not None:
                # 稀疏注意力 Rainfusion
                if rainfusion_config["type"] == "v1":
                    rainfusion_fa = Rainfusion(...)
                    out = rainfusion_fa(q, k, v, ...)
                else:
                    rainfusion_fa_blockwise = Rainfusion_blockwise(...)
                    out, _ = rainfusion_fa_blockwise(q, k, v, ...)
            elif ALGO == 1:
                # Laser 注意力
                out = attention_forward(q, k, v, opt_mode="manual", op_type="ascend_laser_attention")
            elif ALGO == 3:
                # 融合推理注意力 (量化场景)
                out = torch_npu.npu_fused_infer_attention_score(...)
            else:
                # 标准融合注意力
                out = attention_forward(q, k, v, opt_mode="manual", op_type="fused_attn_score")
            return out
        else:
            # 回退到 PyTorch 标准实现
            out = torch.nn.functional.scaled_dot_product_attention(...)
```

#### 1.1.5 WanAttentionBlock 改动

```python
class WanAttentionBlock(nn.Module):
    def __init__(self, ...):
        ...
        # 新增: Attention Cache (用于增量推理)
        self.cache = None
        self.args = None
    
    def forward(self, x, ..., rainfusion_config=None, t_idx=None, b_idx=None):
        # 改动1: 使用 NPU 算子加速
        - with torch.amp.autocast('cuda', dtype=torch.float32):
        + with torch.amp.autocast('cuda', dtype=torch.bfloat16):
        
        # 改动2: Fast LayerNorm 支持
        if FAST_LAYERNORM == 1:
            norm1_out = fast_layernorm(self.norm1, x)
        else:
            norm1_out = self.norm1(x)
        
        # 改动3: 调用带缓存的 attention
        y = self.cache.apply(
            self.self_attn,
            norm1_out * (1 + e[1]) + e[0],
            seq_lens, grid_sizes, freqs,
            rainfusion_config=rainfusion_config,
            t_idx=t_idx, b_idx=b_idx
        )
```

---

### 1.2 generate.py - 主入口

**文件**: `generate.py`

#### 1.2.1 NPU 初始化

```python
# 新增: NPU 基础配置
import torch_npu
torch_npu.npu.set_compile_mode(jit_compile=False)
torch.npu.config.allow_internal_format=False
from torch_npu.contrib import transfer_to_npu
```

#### 1.2.2 分布式初始化

```python
# 新增: 昇腾分布式并行
from wan.distributed.parallel_mgr import ParallelConfig, init_parallel_env, finalize_parallel_env
from wan.distributed.tp_applicator import TensorParallelApplicator
from mindiesd import CacheConfig, CacheAgent
```

#### 1.2.3 推理参数验证

```python
# 新增: 更严格的参数验证
+ assert args.sample_steps >= 1
+ assert args.sample_shift > 0.0
+ assert args.sample_guide_scale > 0.0
+ assert args.frame_num > 1 and (args.frame_num - 1) % 4 == 0  # 帧数必须为 4n+1
```

---

### 1.3 text2video.py / image2video.py - Pipeline

#### 1.3.1 NPU 设备检测

```python
# 检测是否为昇腾910B (设备代号 95)
DEVICE_95 = '95' in torch_npu.npu.get_device_name()

if t5_fsdp or dit_fsdp or use_sp or DEVICE_95:
    self.init_on_cpu = False
```

#### 1.3.2 VAE 并行

```python
# 新增: VAE 空间并行
if use_vae_parallel:
    all_pp_group_ranks = []
    if dist.get_world_size() < 8:
        all_pp_group_ranks.append(list(range(0, dist.get_world_size())))
        set_vae_patch_parallel(self.vae.model, dist.get_world_size(), 1, ...)
    else:
        for i in range(0, dist.get_world_size() // 8):
            all_pp_group_ranks.append(list(range(8 * i, 8 * (i + 1))))
        set_vae_patch_parallel(self.vae.model, 4, 2, ...)
```

#### 1.3.3 DiT 量化支持

```python
# 新增: 量化模型加载
if quant_dit_path:
    from mindiesd import quantize
    quantize(
        model=self.low_noise_model,
        quant_des_path=quant_low_noise_desc_path,
        use_nz=use_nz
    )
```

---

## 二、新增文件

### 2.1 wan/distributed/comm.py

**功能**: NPU 分布式通信原语

```python
def all_to_all_4D(input_, scatter_idx=2, gather_idx=1, group=None):
    """
    用于序列并行的 all-to-all 通信
    
    输入: (bs, seqlen/P, hc, hs)  # 按序列维度分片
    输出: (bs, seqlen, hc/P, hs)  # 按头维度分片
    """
    # 用于 SP (Sequence Parallel) 下的 QKV 交换
```

### 2.2 wan/distributed/parallel_mgr.py

**功能**: 分布式并行管理器

```python
@dataclass
class ParallelConfig:
    tp_degree: int = 1       # Tensor Parallelism
    sp_degree: int = 1       # Sequence Parallelism  
    ulysses_degree: int = 1  # Ulysses Sequence Parallel
    ring_degree: int = 1     # Ring Sequence Parallel
    use_cfg_parallel: bool = False  # Classifier-Free Guidance Parallel

def initialize_model_parallel(
    classifier_free_guidance_degree: int = 1,
    sequence_parallel_degree: int = 1,
    ulysses_degree: int = 1,
    ring_degree: int = 1,
    tensor_parallel_degree: int = 1,
):
    """
    初始化多维并行:
    - TP: 张量并行，模型层内分片
    - SP: 序列并行，序列维度分片
    - CFG: 无分类器引导并行，条件/非条件分片
    """
```

### 2.3 wan/distributed/tp_applicator.py

**功能**: 张量并行应用器

```python
class TensorParallelApplicator:
    """将模型转换为张量并行版本"""
    
    def _apply_tp_to_attention(self, model):
        # 将 Q/K/V/O 线性层替换为 ColumnParallelLinear/RowParallelLinear
        # 实现模型层内分片
    
    def _apply_tp_to_ffn(self, model):
        # 将 FFN 替换为张量并行版本
```

### 2.4 wan/utils/rainfusion.py

**功能**: Rainfusion 稀疏注意力机制

```python
class Rainfusion(torch.nn.Module):
    """
    华为自研的稀疏注意力机制
    
    原理: 在高噪声阶段 (去噪前期)，视频帧之间的依赖较弱，
    可以使用稀疏注意力减少计算量。
    
    参数:
        grid_size: latent 的 THW 网格大小
        skip_timesteps: 从第几步开始启用稀疏注意力
        sparsity: 稀疏度 [0, 1]
    """
    
    def __init__(self, grid_size, skip_timesteps=20, sparsity=0.5):
        self.bandwidth = 1 - sqrt(sparsity)  # 带宽
        self.use_rainfusion = False if sparsity == 1.0 else True
```

### 2.5 wan/vae_patch_parallel.py

**功能**: VAE 空间并行

```python
class Parallel_VAE_SP:
    """
    VAE 空间并行
    
    将 VAE 的编码器/解码器按空间维度分片
    """
    
    def __init__(self, h_split=1, w_split=1, all_pp_group_ranks=None):
        # 按 H 和 W 维度创建进程组
        # 实现行方向和列方向的集合通信
```

---

## 三、环境变量配置

MindIE 通过环境变量控制各种优化特性：

| 环境变量 | 说明 | 示例值 |
|---------|------|--------|
| `FAST_LAYERNORM` | 启用 Fast LayerNorm | 0/1 |
| `ROPE_OPT` | 启用 RoPE 优化 | 0/1 |
| `USE_SUB_HEAD` | 子序列并行头数 | 1/2/4/... |
| `ALGO` | Attention 算法选择 | 0/1/2/3 |
| `T5_LOAD_CPU` | T5 是否加载到 CPU | 0/1 |

**ALGO 取值说明**:
- `0`: 标准融合 Attention (`fused_attn_score`)
- `1`: Laser Attention
- `2`: (保留)
- `3`: 融合推理 Attention (量化场景)

---

## 四、性能优化技术

### 4.1 算子融合

| 原实现 | 优化后 | 收益 |
|--------|--------|------|
| Python 循环 + PyTorch算子 | `rotary_position_embedding` 融合算子 | 减少 kernel 启动 |
| RMSNorm 手写 | `npu_rms_norm` | 减少 memory copy |
| Flash Attention | `attention_forward` (华为融合) | 更好 NPU 优化 |

### 4.2 分布式并行策略

```
多卡并行策略示例 (8卡):

┌─────────────────────────────────────────────────────────────┐
│                     8 × NPU                                 │
├─────────────────────────────────────────────────────────────┤
│  TP=2, SP=2, CFG=2                                        │
│                                                             │
│  ┌──────────────┬──────────────┬──────────────┬────────────┐│
│  │ GPU 0       │ GPU 1       │ GPU 2       │ GPU 3     ││
│  │ TP=0, SP=0  │ TP=1, SP=0  │ TP=0, SP=1  │ TP=1, SP=1││
│  │ CFG=0       │ CFG=0       │ CFG=1       │ CFG=1     ││
│  └──────────────┴──────────────┴──────────────┴────────────┘│
│  ...                                                        │
└─────────────────────────────────────────────────────────────┘

- TP (Tensor Parallel): 模型层内分片 (QKV, FFN)
- SP (Sequence Parallel): 序列维度分片
- CFG (Classifier-Free Guidance): 条件/非条件分支并行
```

### 4.3 Rainfusion 稀疏注意力

```python
# 在高噪声阶段 (t > 800) 启用稀疏注意力
if current_timestep > skip_timesteps:
    # 带宽 = 1 - sqrt(sparsity)
    # 只计算局部窗口的注意力
    attention = sparse_attention(q, k, v, bandwidth=0.3)
else:
    # 低噪声阶段使用全量注意力
    attention = full_attention(q, k, v)
```

### 4.4 量化推理 (W4A4)

```python
# W4A4: 权重4bit，激活8bit
# 显著减少显存和计算量
from mindiesd import quantize

quantize(
    model=diy_model,
    quant_des_path="quant_config.json",
    use_nz=True  # 非零稀疏
)
```

---

## 五、其他文件详细改动

### 5.1 VAE 相关文件 (vae2_1.py, vae2_2.py)

**改动**: CausalConv3d padding 顺序调整

```python
# 原版
self._padding = (self.padding[2], self.padding[2], self.padding[1], self.padding[1], 2 * self.padding[0], 0)
self.padding = (0, 0, 0)

# MindIE版 - 调整padding顺序适应NPU
self._padding = (0, 0, 0, 0, 2 * self.padding[0], 0)
self.padding = (0, self.padding[1], self.padding[2])
```

### 5.2 T5 编码器 (t5.py)

```python
# 新增导入
import torch_npu
from torch_npu.contrib import transfer_to_npu
```

### 5.3 分布式工具 (util.py)

**新增**: 并行组生成函数

```python
def generate_masked_orthogonal_rank_groups(world_size, parallel_size, mask):
    """生成正交并行组，用于TP/SP/CFG多维并行"""
```

### 5.4 FSDP (fsdp.py)

```python
# 移除 use_lora 参数
- use_lora=False
- use_orig_params=True if use_lora else False
```

### 5.5 序列并行 (sequence_parallel.py)

```python
# RoPE 使用华为融合算子替代手写实现
# 导入 Rainfusion 和 LongContextAttention
```

### 5.6 采样器 (fm_solvers_unipc.py)

```python
# 矩阵求解方法调整 (NPU兼容性)
# torch.linalg.solve → torch.inverse + torch.matmul
```

### 5.7 工具函数 (utils.py)

```python
# 移除 merge_video_audio 函数 (S2V模块被裁剪)
```

### 5.8 配置 (configs/__init__.py)

```python
# 移除 animate-14B 和 s2v-14B 模型配置
# 调整分辨率配置: 1024*704 → 432*768
# 新增 OPTIMAL_PARALLEL 最优并行配置
```

---

## 六、新增模块详解

### 6.1 attn_layer.py
```python
class xFuserLongContextAttention:
    """Long Context Attention 封装 - 用于长序列场景"""
```

### 6.2 rainfusion_blockwise.py
```python
class Rainfusion_blockwise:
    """分块Rainfusion - 按块稀疏，更适合超大分辨率"""
```

### 6.3 quant_wan22.py
```python
# 量化推理脚本 - W4A4量化
```

### 6.4 group_coordinator.py
```python
class GroupCoordinator:
    """进程组协调器 - 封装ProcessGroup，提供通信操作"""
```

---

## 七、模型裁剪

| 被移除模块 | 说明 |
|-----------|------|
| `wan/animate.py` | 角色动画 |
| `wan/speech2video.py` | 语音到视频 |
| `wan/modules/s2v/` | S2V模块 |
| `wan/modules/animate/` | Animation模块 |

---

## 八、与原版 Wan 2.2 的区别

| 特性 | 原版 (CUDA/GPU) | MindIE (NPU) |
|------|-----------------|---------------|
| 设备 | CUDA | NPU (Ascend) |
| 分布式 | 基础 FSDP | TP/SP/CFG 多维并行 |
| Attention | Flash Attention | 多种选择 (Fusion/Laser/Rainfusion) |
| LayerNorm | PyTorch 实现 | NPU 融合算子 |
| RoPE | 手写 Python | 华为融合算子 |
| 量化 | 不支持 | W4A4 量化 |
| VAE 并行 | 不支持 | 空间并行 |

---

## 六、总结

MindIE 版本的 Wan 2.2 主要做了以下适配工作：

1. **算子层**: 使用华为自研的 NPU 算子替代原有 CUDA 实现
2. **框架层**: 适配昇腾的分布式并行策略 (TP/SP/CFG)
3. **算法层**: 引入 Rainfusion 稀疏注意力等优化算法
4. **部署层**: 支持量化推理，降低显存和计算需求

这些改动使得 Wan 2.2 能够在华为昇腾 NPU 上高效运行，满足国产化 AI 基础设施的需求。

---

*文档更新时间: 2026年3月*
