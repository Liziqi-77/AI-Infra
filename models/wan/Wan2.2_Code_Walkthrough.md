# Wan 2.2 核心模块代码走读

## 目录
1. [VAE 模块 (视频压缩/解压)](#1-vae-模块)
2. [T5 文本编码器](#2-t5-文本编码器)
3. [DiT 主干网络](#3-di t-主干网络)
4. [注意力机制](#4-注意力机制)
5. [文本到视频 Pipeline](#5-文本到视频-pipeline)
6. [Flow Matching 采样器](#6-flow-matching-采样器)
7. [配置文件](#7-配置文件)

---

## 1. VAE 模块

**文件**: `wan/modules/vae2_2.py`

### 1.1 核心类: `Wan2_2_VAE`

```python
class Wan2_2_VAE:
    def __init__(self, z_dim=48, c_dim=160, vae_pth=None, ...):
        # 初始化VAE模型
        self.model = _video_vae(...)
        # 归一化参数 (均值和标准差)
        self.scale = [mean, 1.0 / std]
    
    def encode(self, videos):
        """视频 -> Latent"""
        # 对视频进行patchify (2x压缩)
        x = patchify(x, patch_size=2)
        # 编码成latent
        mu = self.model.encode(x, self.scale)
        return mu
    
    def decode(self, zs):
        """Latent -> 视频"""
        # 解码
        out = self.model.decode(z, self.scale)
        # 恢复patchify
        out = unpatchify(out, patch_size=2)
        return out
```

### 1.2 3D VAE 架构

```python
class WanVAE_(nn.Module):
    def __init__(self, dim=160, z_dim=16, dim_mult=[1,2,4,4], ...):
        # 编码器: 视频 -> latent
        self.encoder = Encoder3d(dim, z_dim*2, dim_mult, ...)
        
        # 解码器: latent -> 视频  
        self.decoder = Decoder3d(dec_dim, z_dim, dim_mult, ...)
```

### 1.3 关键组件

#### CausalConv3d (因果3D卷积)
```python
class CausalConv3d(nn.Conv3d):
    """确保时间维度的因果性 - 当前帧只能参考之前的帧"""
    
    def forward(self, x, cache_x=None):
        # 在时间维度前面填充，保持因果关系
        x = F.pad(x, padding)  # padding格式: (left, right, top, bottom, front, back)
        return super().forward(x)
```

#### ResidualBlock (残差块)
```python
class ResidualBlock(nn.Module):
    def __init__(self, in_dim, out_dim, dropout=0.0):
        self.residual = nn.Sequential(
            RMS_norm(in_dim),    # 归一化
            nn.SiLU(),           # 激活函数
            CausalConv3d(in_dim, out_dim, 3, padding=1),  # 卷积
            RMS_norm(out_dim),
            nn.SiLU(),
            nn.Dropout(dropout),
            CausalConv3d(out_dim, out_dim, 3, padding=1),
        )
        self.shortcut = CausalConv3d(in_dim, out_dim, 1) if in_dim != out_dim else nn.Identity()
    
    def forward(self, x):
        return self.shortcut(x) + self.residual(x)  # 残差连接
```

### 1.4 Patchify/Unpatchify

```python
def patchify(x, patch_size):
    """将视频分割成patches，实现空间压缩"""
    # 例如: [B, C, T, H, W] -> [B, C', T, H/p, W/p]
    x = rearrange(x, "b c t (h q) (w r) -> b (c r q) t h w", q=patch_size, r=patch_size)
    return x

def unpatchify(x, patch_size):
    """将patches合并回视频"""
    x = rearrange(x, "b (c r q) t h w -> b c t (h q) (w r)", ...)
    return x
```

### 1.5 压缩比计算

```
输入: 720P 视频 (1280×720), 81帧
经过 VAE: 16×16×4 压缩 → Latent: 20×45×5
经过 Patchify(2×): 10×22×5
总压缩: 64×
```

---

## 2. T5 文本编码器

**文件**: `wan/modules/t5.py`

### 2.1 核心类: `T5EncoderModel`

```python
class T5EncoderModel:
    def __init__(self, text_len, dtype, device, checkpoint_path, tokenizer_path):
        # 加载 UM-T5 XXL 模型
        model = umt5_xxl(encoder_only=True, dtype=dtype, device=device)
        model.load_state_dict(torch.load(checkpoint_path))
        
        # 初始化分词器
        self.tokenizer = HuggingfaceTokenizer(name=tokenizer_path, seq_len=text_len)
    
    def __call__(self, texts, device):
        # 1. Tokenize: 文本 -> token ids
        ids, mask = self.tokenizer(texts, return_mask=True, add_special_tokens=True)
        
        # 2. 编码: token ids -> embeddings
        seq_lens = mask.gt(0).sum(dim=1).long()  # 实际序列长度
        context = self.model(ids, mask)
        
        # 3. 截取实际长度
        return [u[:v] for u, v in zip(context, seq_lens)]
```

### 2.2 UMT5 模型配置

```python
def umt5_xxl(**kwargs):
    cfg = dict(
        vocab_size=256384,    # 词表大小
        dim=4096,             # 隐藏维度
        dim_attn=4096,        # 注意力维度
        dim_ffn=10240,        # FFN中间维度
        num_heads=64,        # 注意力头数
        encoder_layers=24,   # 编码器层数
        decoder_layers=24,
        num_buckets=32,      # 相对位置编码bucket数
    )
    return _t5('umt5-xxl', **cfg)
```

### 2.3 T5 编码器结构

```python
class T5Encoder(nn.Module):
    def __init__(self, vocab, dim, dim_attn, dim_ffn, num_heads, num_layers, ...):
        self.token_embedding = nn.Embedding(vocab, dim)  # 词嵌入
        self.pos_embedding = T5RelativeEmbedding(...)       # 位置嵌入
        self.blocks = nn.ModuleList([
            T5SelfAttention(...) for _ in range(num_layers)
        ])
        self.norm = T5LayerNorm(dim)
    
    def forward(self, ids, mask=None):
        x = self.token_embedding(ids)  # [B, L, D]
        
        for block in self.blocks:
            x = block(x, mask)  # 多层Transformer
        
        x = self.norm(x)
        return x
```

### 2.4 注意力机制 (T5SelfAttention)

```python
class T5SelfAttention(nn.Module):
    def forward(self, x, mask=None, pos_bias=None):
        # Pre-LN 结构
        x = fp16_clamp(x + self.attn(self.norm1(x), mask=mask, pos_bias=e))
        x = fp16_clamp(x + self.ffn(self.norm2(x)))
        return x
```

---

## 3. DiT 主干网络

**文件**: `wan/modules/model.py`

### 3.1 核心类: `WanModel`

```python
class WanModel(ModelMixin, ConfigMixin):
    def __init__(self,
                 model_type='t2v',      # 't2v', 'i2v', 'ti2v', 's2v'
                 patch_size=(1,2,2),    # 3D patch大小
                 text_len=512,          # 文本最大长度
                 in_dim=16,             # 输入通道数 (VAE latent C)
                 dim=2048,              # Transformer隐藏维度
                 ffn_dim=8192,          # FFN中间维度
                 num_heads=16,          # 注意力头数
                 num_layers=32,         # Transformer层数
                 ...):
        
        # 1. Patch Embedding: 将视频转为一维序列
        self.patch_embedding = nn.Conv3d(in_dim, dim, kernel_size=patch_size, stride=patch_size)
        
        # 2. Text Embedding: 文本条件投影
        self.text_embedding = nn.Sequential(
            nn.Linear(text_dim, dim), nn.GELU(), nn.Linear(dim, dim)
        )
        
        # 3. Time Embedding: 时间步嵌入
        self.time_embedding = nn.Sequential(
            nn.Linear(freq_dim, dim), nn.SiLU(), nn.Linear(dim, dim)
        )
        self.time_projection = nn.Sequential(nn.SiLU(), nn.Linear(dim, dim * 6))
        
        # 4. Transformer Blocks
        self.blocks = nn.ModuleList([
            WanAttentionBlock(dim, ffn_dim, num_heads, ...) 
            for _ in range(num_layers)
        ])
        
        # 5. Head: 输出层
        self.head = Head(dim, out_dim, patch_size)
```

### 3.2 前向传播

```python
def forward(self, x, t, context, seq_len, y=None):
    # 1. Patchify: [C, F, H, W] -> [C', F', H', W']
    x = [self.patch_embedding(u.unsqueeze(0)) for u in x]  # list of tensors
    
    # 2. 记录grid尺寸，用于后续unpatchify
    grid_sizes = torch.stack([torch.tensor(u.shape[2:], dtype=torch.long) for u in x])
    
    # 3. Flatten + Transpose: [B, C', F', H', W'] -> [B, L, C]
    x = [u.flatten(2).transpose(1, 2) for u in x]
    
    # 4. 时间步嵌入
    e = self.time_embedding(sinusoidal_embedding_1d(self.freq_dim, t))
    e0 = self.time_projection(e)  # [B, L, 6, C]
    
    # 5. 文本条件嵌入
    context = self.text_embedding(torch.stack([...]))  # [B, 512, C]
    
    # 6. 通过Transformer blocks
    for block in self.blocks:
        x = block(x, e0, seq_lens, grid_sizes, freqs, context, context_lens)
    
    # 7. 输出头
    x = self.head(x, e)
    
    # 8. Unpatchify: [B, L, C] -> [B, C', F', H', W']
    x = self.unpatchify(x, grid_sizes)
    return [u.float() for u in x]
```

### 3.3 WanAttentionBlock

```python
class WanAttentionBlock(nn.Module):
    def __init__(self, dim, ffn_dim, num_heads, ...):
        self.norm1 = WanLayerNorm(dim)           # Pre-LN
        self.self_attn = WanSelfAttention(...)   # 自注意力
        self.norm3 = WanLayerNorm(dim)           # Pre-LN (可选)
        self.cross_attn = WanCrossAttention(...)  # 交叉注意力 (文本)
        self.norm2 = WanLayerNorm(dim)           # Pre-LN
        self.ffn = nn.Sequential(...)             # 前馈网络
        
        # 调制参数 (用于AdaLN)
        self.modulation = nn.Parameter(torch.randn(1, 6, dim) / dim**0.5)
    
    def forward(self, x, e, seq_lens, grid_sizes, freqs, context, context_lens):
        # 1. 计算调制因子
        e = (self.modulation.unsqueeze(0) + e).chunk(6, dim=2)
        
        # 2. 自注意力 + AdaLN调制
        y = self.self_attn(self.norm1(x).float() * (1 + e[1]) + e[0], ...)
        x = x + y * e[2]
        
        # 3. 交叉注意力 + FFN
        x = cross_attn_ffn(x, context, context_lens, e)
        
        return x
```

---

## 4. 注意力机制

**文件**: `wan/modules/attention.py`

### 4.1 Flash Attention

```python
def flash_attention(q, k, v, q_lens=None, k_lens=None, ...):
    """
    使用Flash Attention加速注意力计算
    
    q: [B, Lq, Nq, C]  # Query
    k: [B, Lk, Nk, C]  # Key
    v: [B, Lk, Nk, C]  # Value
    """
    
    # 1. 预处理: 填充到统一长度
    q = half(torch.cat([u[:v] for u, v in zip(q, q_lens)]))
    k = half(torch.cat([u[:v] for u, v in zip(k, k_lens)]))
    v = half(torch.cat([u[:v] for u, v in zip(v, k_lens)]))
    
    # 2. 调用Flash Attention
    if FLASH_ATTN_3_AVAILABLE:
        x = flash_attn_interface.flash_attn_varlen_func(
            q=q, k=k, v=v,
            cu_seqlens_q=...,  # 序列长度累积
            cu_seqlens_k=...,
            ...
        )
    else:
        x = flash_attn.flash_attn_varlen_func(...)
    
    return x
```

### 4.2 Rotary Position Embedding (RoPE)

```python
@torch.amp.autocast('cuda', enabled=False)
def rope_apply(x, grid_sizes, freqs):
    """应用旋转位置编码"""
    
    # 分离频率
    freqs = freqs.split([c - 2*(c//3), c//3, c//3], dim=1)
    
    # 对每个样本应用RoPE
    for i, (f, h, w) in enumerate(grid_sizes.tolist()):
        # 复数乘法实现旋转
        x_i = torch.view_as_complex(x[i].reshape(seq_len, n, -1, 2))
        freqs_i = ...  # 预计算的频率
        x_i = torch.view_as_real(x_i * freqs_i)  # 旋转
        
    return torch.stack(outputs)
```

---

## 5. 文本到视频 Pipeline

**文件**: `wan/text2video.py`

### 5.1 核心类: `WanT2V`

```python
class WanT2V:
    def __init__(self, config, checkpoint_dir, ...):
        # 1. 文本编码器 (T5)
        self.text_encoder = T5EncoderModel(
            text_len=config.text_len,
            checkpoint_path=os.path.join(checkpoint_dir, config.t5_checkpoint),
            ...
        )
        
        # 2. VAE (视频编解码)
        self.vae = Wan2_1_VAE(
            vae_pth=os.path.join(checkpoint_dir, config.vae_checkpoint),
            device=self.device
        )
        
        # 3. DiT 模型 (两个专家)
        # 低噪声专家: 负责后期去噪 (细节纹理)
        self.low_noise_model = WanModel.from_pretrained(
            checkpoint_dir, subfolder=config.low_noise_checkpoint)
        
        # 高噪声专家: 负责早期去噪 (构图运动)
        self.high_noise_model = WanModel.from_pretrained(
            checkpoint_dir, subfolder=config.high_noise_checkpoint)
```

### 5.2 生成流程

```python
def generate(self, input_prompt, size=(1280,720), frame_num=81, ...):
    # ==================== 步骤1: 文本编码 ====================
    if not self.t5_cpu:
        self.text_encoder.model.to(self.device)
        context = self.text_encoder([input_prompt], self.device)       # 正向提示
        context_null = self.text_encoder([n_prompt], self.device)       # 负向提示
    else:
        context = self.text_encoder([input_prompt], torch.device('cpu'))
    
    # ==================== 步骤2: 初始化噪声 ====================
    # 计算目标latent形状
    target_shape = (
        self.vae.model.z_dim,     # 通道数 (如48)
        (frame_num - 1) // vae_stride[0] + 1,  # 时间维度
        size[1] // vae_stride[1],  # 高度
        size[0] // vae_stride[2]   # 宽度
    )
    # 例如: 720P, 81帧 -> (48, 20, 45, 80)
    
    noise = [torch.randn(target_shape, dtype=torch.float32, device=self.device)]
    
    # ==================== 步骤3: 设置采样器 ====================
    boundary = self.boundary * self.num_train_timesteps  # 1000 * 0.875 = 875
    
    sample_scheduler = FlowUniPCMultistepScheduler(...)
    sample_scheduler.set_timesteps(sampling_steps, device=self.device, shift=shift)
    timesteps = sample_scheduler.timesteps  # e.g., [980, 960, ..., 20]
    
    # ==================== 步骤4: 去噪循环 ====================
    latents = noise
    
    for _, t in enumerate(timesteps):
        # 选择当前时间步使用的模型
        model = self._prepare_model_for_timestep(t, boundary, offload_model)
        
        # 根据当前时间步选择引导强度
        sample_guide_scale = guide_scale[1] if t.item() >= boundary else guide_scale[0]
        
        # 条件预测 (使用提示)
        noise_pred_cond = model(latent_model_input, t=timestep, **arg_c)[0]
        
        # 无条件预测 (使用负提示)
        noise_pred_uncond = model(latent_model_input, t=timestep, **arg_null)[0]
        
        # Classifier-Free Guidance
        noise_pred = noise_pred_uncond + sample_guide_scale * (noise_pred_cond - noise_pred_uncond)
        
        # 更新latent
        temp_x0 = sample_scheduler.step(noise_pred, t, latents[0], ...)
        latents = [temp_x0.squeeze(0)]
    
    # ==================== 步骤5: VAE解码 ====================
    videos = self.vae.decode(latents)
    
    return videos[0]  # [C, F, H, W]
```

### 5.3 专家切换机制

```python
def _prepare_model_for_timestep(self, t, boundary, offload_model):
    """根据时间步选择使用哪个专家模型"""
    
    if t.item() >= boundary:
        # 早期阶段 (高噪声): 使用高噪声专家
        required_model_name = 'high_noise_model'
        offload_model_name = 'low_noise_model'
    else:
        # 后期阶段 (低噪声): 使用低噪声专家
        required_model_name = 'low_noise_model'
        offload_model_name = 'high_noise_model'
    
    # 模型卸载/加载
    if offload_model:
        getattr(self, offload_model_name).to('cpu')
        getattr(self, required_model_name).to(self.device)
    
    return getattr(self, required_model_name)
```

---

## 6. Flow Matching 采样器

**文件**: `wan/utils/fm_solvers.py`

### 6.1 核心概念

Flow Matching 不预测噪声，而是预测速度场 (velocity field)：

```
x_t = (1-t) * x_0 + t * noise  # 插值
v = dx/dt = noise - x_0        # 速度
```

### 6.2 采样调度

```python
def get_sampling_sigmas(sampling_steps, shift):
    """计算采样sigma (噪声水平)"""
    sigma = np.linspace(1, 0, sampling_steps + 1)[:sampling_steps]
    sigma = (shift * sigma / (1 + (shift - 1) * sigma))
    return sigma
```

### 6.3 UniPC Scheduler

```python
class FlowUniPCMultistepScheduler:
    def set_timesteps(self, num_steps, device, shift=1.0):
        # 线性噪声调度
        alphas = np.linspace(1, 1/1000, 1000)[::-1]
        sigmas = 1 - alphas
        
        # Shift调整
        sigmas = shift * sigmas / (1 + (shift-1) * sigmas)
        
        self.sigmas = torch.tensor(sigmas).to(device)
        self.timesteps = self.sigmas * 1000
    
    def step(self, model_output, timestep, sample, ...):
        # Flow matching 步骤
        # v = model_output (预测的速度)
        # x_t+1 = x_t + v * dt
        
        prev_sample = sample + model_output * (timestep - prev_timestep)
        return prev_sample
```

---

## 7. 配置文件

**文件**: `wan/configs/wan_t2v_A14B.py`

### 7.1 T2V-A14B 配置

```python
t2v_A14B = EasyDict(__name__='Config: Wan T2V A14B')

# T5 文本编码器
t2v_A14B.t5_checkpoint = 'models_t5_umt5-xxl-enc-bf16.pth'
t2v_A14B.t5_tokenizer = 'google/umt5-xxl'

# VAE
t2v_A14B.vae_checkpoint = 'Wan2.1_VAE.pth'
t2v_A14B.vae_stride = (4, 8, 8)  # 时间4x, 空间8x8

# DiT Transformer (A14B = 14B参数)
t2v_A14B.patch_size = (1, 2, 2)
t2v_A14B.dim = 5120          # 隐藏维度
t2v_A14B.ffn_dim = 13824     # FFN中间维度
t2v_A14B.num_heads = 40      # 注意力头数
t2v_A14B.num_layers = 40     # 层数

# 专家模型
t2v_A14B.low_noise_checkpoint = 'low_noise_model'    # 低噪声专家
t2v_A14B.high_noise_checkpoint = 'high_noise_model'  # 高噪声专家

# 推理参数
t2v_A14B.sample_shift = 12.0     # 时间偏移
t2v_A14B.sample_steps = 40       # 采样步数
t2v_A14B.boundary = 0.875        # 专家切换边界 (1000 * 0.875 = 875)
t2v_A14B.sample_guide_scale = (3.0, 4.0)  # (低噪声引导, 高噪声引导)
```

---

## 8. 完整数据流

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                           Wan 2.2 推理完整流程                               │
└─────────────────────────────────────────────────────────────────────────────┘

输入: "一只猫在草地上奔跑"

  ┌─────────────┐
  │ 1. 文本输入  │
  └──────┬──────┘
         │
         ▼
  ┌──────────────────────────────────────┐
  │ 2. T5 编码器 (UMT5-XXL)              │
  │    "一只猫在草地上奔跑" -> [L, 4096]   │
  │    (L = 实际token数量)                │
  └──────┬───────────────────────────────┘
         │
         ▼
  ┌──────────────────────────────────────┐
  │ 3. 初始化噪声 Latent                  │
  │    shape: (48, 20, 45, 80)            │
  │    720P, 81帧, VAE压缩后              │
  └──────┬───────────────────────────────┘
         │
         ▼
  ┌──────────────────────────────────────┐
  │ 4. 去噪循环 (40步)                    │
  │                                      │
  │  for t in [980, 960, ..., 20]:       │
  │                                      │
  │    判断 t >= 875 ?                   │
  │    ├─ Yes: 使用 high_noise_model     │
  │    └─ No:  使用 low_noise_model     │
  │                                      │
  │    noise_pred = model(latent, t,     │
  │                       text_emb)      │
  │                                      │
  │    # CFG                            │
  │    noise_pred = noise_uncond +      │
  │                 scale * (cond - un) │
  │                                      │
  │    latent = scheduler.step(         │
  │                   noise_pred, t,    │
  │                   latent)           │
  └──────┬───────────────────────────────┘
         │
         ▼
  ┌──────────────────────────────────────┐
  │ 5. VAE 解码                          │
  │    latent (48,20,45,80) ->           │
  │    video (3,81,720,1280)             │
  └──────┬───────────────────────────────┘
         │
         ▼
  输出: 视频 tensor [3, 81, 720, 1280]
```

---

## 9. 参数计算

### 9.1 DiT 模型参数量

```
A14B 模型配置:
- dim = 5120
- num_heads = 40  
- head_dim = dim / num_heads = 5120 / 40 = 128
- num_layers = 40
- ffn_dim = 13824

每个AttentionBlock参数量:
- Q/K/V: 3 * dim * dim = 3 * 5120 * 5120 = 80M
- O: dim * dim = 26M
- FFN: 2 * dim * ffn_dim = 2 * 5120 * 13824 = 141M
- LayerNorm: 忽略
总计每层: ~250M

总参数量: 250M * 40 = 10B (单专家)
MoE双专家: 20B (但推理只激活14B)
```

### 9.2 Latent 形状计算

```
输入: 1280x720 视频, 81帧

VAE编码后 (stride=4,8,8):
- 时间: 81 / 4 = 20.25 -> 20
- 高度: 720 / 8 = 90
- 宽度: 1280 / 8 = 160
- 通道: 48

Latent shape: (48, 20, 90, 160)

Patchify后 (patch_size=1,2,2):
- 时间: 20 / 1 = 20
- 高度: 90 / 2 = 45
- 宽度: 160 / 2 = 80
- 通道: 48 * 1 * 2 * 2 = 192

最终序列长度: 20 * 45 * 80 = 72,000
```

---

*文档更新时间: 2026年3月*
