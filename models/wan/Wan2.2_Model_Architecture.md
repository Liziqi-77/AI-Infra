# Wan 2.2 模型结构详细解析

## 1. 概述

Wan 2.2 是阿里巴巴通义万相团队开发的新一代开源AI视频生成模型，于2025年7月发布。该模型在视频生成领域引入了多项创新技术，是目前最强大的开源视频生成模型之一。

### 1.1 核心特性

| 特性 | 描述 |
|------|------|
| **MoE架构** | 首次将混合专家机制引入视频扩散模型 |
| **电影级美学控制** | 支持灯光、构图、对比度、色彩等精细控制 |
| **复杂运动生成** | 增强的运动流畅性和可控性 |
| **消费级GPU支持** | 5B版本可在RTX 4090上运行 |
| **LoRA微调支持** | 支持轻量级模型定制 |

---

## 2. 模型系列

Wan 2.2 提供三种主要模型变体：

### 2.1 Wan2.2-T2V-A14B (文本到视频)
- **参数量**: 27B总参数，14B激活
- **架构**: MoE (Mixture-of-Experts)
- **分辨率**: 最高 1280×720 (720P)
- **帧率**: 24/30 FPS
- **最大时长**: 5秒
- **特点**: 最高质量的文本到视频生成，适合专业工作流

### 2.2 Wan2.2-I2V-A14B (图像到视频)
- **参数量**: 27B总参数，14B激活
- **架构**: MoE
- **分辨率**: 最高 1280×720
- **输入**: 静态图像 → 动态视频
- **特点**: 优秀的运动控制，图像动画化

### 2.3 Wan2.2-TI2V-5B (统一模型)
- **参数量**: 5B (密集模型，非MoE)
- **分辨率**: 1280×704
- **帧率**: 24 FPS
- **显存要求**: 8GB VRAM
- **特点**: 可在消费级GPU上运行，适合快速原型开发

---

## 3. 核心架构详解

### 3.1 整体架构图

```
┌─────────────────────────────────────────────────────────────────┐
│                         Wan 2.2 架构                             │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│   ┌──────────────┐      ┌─────────────────────────────────┐    │
│   │   Text Input │      │        Image Input              │    │
│   │   (UMT5)     │      │        (可选)                   │    │
│   └──────┬───────┘      └─────────────┬─────────────────┘    │
│          │                             │                        │
│          ▼                             ▼                        │
│   ┌──────────────────────────────────────────────┐             │
│   │              文本编码器 (UMT5)                  │             │
│   │         512 tokens 固定长度                    │             │
│   └──────────────────┬───────────────────────────┘             │
│                      │                                         │
│                      ▼                                         │
│   ┌──────────────────────────────────────────────┐             │
│   │              VAE 编码器 (3D VAE)              │             │
│   │         16×16×4 压缩率 (t/h/w)                │             │
│   │         额外patchify: 总压缩率 64             │             │
│   └──────────────────┬───────────────────────────┘             │
│                      │                                         │
│                      ▼                                         │
│   ┌──────────────────────────────────────────────┐             │
│   │           噪声注入 + 条件嵌入                  │             │
│   │         (与文本embedding拼接)                 │             │
│   └──────────────────┬───────────────────────────┘             │
│                      │                                         │
│                      ▼                                         │
│   ┌──────────────────────────────────────────────┐             │
│   │          Flow Matching 主干网络               │             │
│   │     ┌─────────────────────────────────┐      │             │
│   │     │    MoE Transformer Block       │      │             │
│   │     │  ┌─────────┐    ┌──────────┐  │      │             │
│   │     │  │High-Noise│    │Low-Noise  │  │      │             │
│   │     │  │ Expert   │    │  Expert   │  │      │             │
│   │     │  │ (14B)    │    │  (14B)    │  │      │             │
│   │     │  └─────────┘    └──────────┘  │      │             │
│   │     │      │              │         │      │             │
│   │     │      └──────────────┘         │      │             │
│   │     │         SNR 切换机制           │      │             │
│   │     └─────────────────────────────────┘      │             │
│   └──────────────────┬───────────────────────────┘             │
│                      │                                         │
│                      ▼                                         │
│   ┌──────────────────────────────────────────────┐             │
│   │              VAE 解码器                       │             │
│   │         latent → 像素视频                     │             │
│   └──────────────────────────────────────────────┘             │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

### 3.2 文本编码器 (UMT5)

Wan 2.2 使用 **UMT5** (Unified Multilingual Text-to-Text Transformer) 作为文本编码器：

- **最大输入长度**: 512 tokens
- **功能**: 将用户文本提示转换为模型可理解的embedding
- **特点**: 多语言支持，强大的语义理解能力

```python
# 伪代码示例
text_encoder = UMT5Encoder()
text_embedding = text_encoder.encode(prompt)  # [batch, 512, hidden_dim]
```

### 3.3 VAE (变分自编码器)

Wan 2.2 采用 **3D VAE** 进行视频压缩：

| 参数 | 值 |
|------|-----|
| 时序压缩 | 16× |
| 高度压缩 | 16× |
| 宽度压缩 | 4× |
| 总压缩率 | 16×16×4 = 1024 |
| 额外patchify | 16× 压缩 |
| **有效压缩率** | **64** |

**优势**:
- 显著减少显存需求和计算时间
- 720P视频可压缩为紧凑的latent表示
- 5秒视频(81帧) → 5×5 latent空间

```python
# VAE 编码过程
video = load_video(...)  # [B, C, T, H, W]
latent = vae.encode(video)  # [B, C', T', H', W']
# 720P (1280x720) -> latent (20x45)

# VAE 解码过程
video = vae.decode(latent)
```

### 3.4 MoE (混合专家) 架构 - 核心创新

这是Wan 2.2最重要的架构创新，首次将MoE引入视频扩散模型：

#### 3.4.1 双专家设计

```
┌────────────────────────────────────────────────────┐
│              SNR (信噪比) 决定专家切换               │
│                                                    │
│   高噪声阶段 ────────────────> 低噪声阶段          │
│                                                    │
│   ┌─────────────────┐        ┌─────────────────┐   │
│   │  High-Noise     │        │  Low-Noise      │   │
│   │  Expert         │  SNR   │  Expert         │   │
│   │  (高噪声专家)    │ 切换   │  (低噪声专家)    │   │
│   │                 │        │                 │   │
│   │ • 构图布局      │        │ • 细节纹理      │   │
│   │ • 整体结构      │        │ • 光照优化      │   │
│   │ • 运动规划      │        │ • 色彩调整      │   │
│   │ • 空间关系      │        │ • 运动一致性    │   │
│   └─────────────────┘        └─────────────────┘   │
│                                                    │
│   参数: 14B              参数: 14B                  │
│   激活: 14B              激活: 14B                  │
└────────────────────────────────────────────────────┘
```

#### 3.4.2 SNR切换机制

**信号-to-噪声比 (SNR)** 是判断当前去噪阶段的关键指标：

```python
# SNR 计算伪代码
def compute_snr(noise, signal):
    """计算信噪比"""
    signal_power = torch.mean(signal ** 2)
    noise_power = torch.mean(noise ** 2)
    return signal_power / (noise_power + 1e-8)

# 专家切换逻辑
def select_expert(latent, timestep, threshold=0.5):
    """根据SNR选择合适的专家"""
    noise_level = compute_snr(latent, original_latent)
    
    if noise_level > threshold:
        return "high_noise_expert"  # 早期去噪阶段
    else:
        return "low_noise_expert"   # 后期精修阶段
```

#### 3.4.3 MoE的优势

| 指标 | 传统Dense模型 | Wan 2.2 MoE |
|------|---------------|-------------|
| 总参数量 | 14B | 27B |
| 激活参数 | 14B | 14B |
| 推理成本 | 1× | ~1× |
| 模型容量 | 1× | ~2× |
| 输出质量 | 基准 | 显著提升 |

### 3.5 Flow Matching (流匹配)

Wan 2.2 使用 **Flow Matching** 而非传统的DDPM/DDIM采样：

```python
# Flow Matching 核心思想
# 不预测噪声，而是学习将噪声流向数据分布

class FlowMatchingBlock(nn.Module):
    def forward(self, x, timestep, condition):
        # 预测速度场 velocity field
        velocity = self.net(x, timestep, condition)
        # 通过常微分方程积分得到去噪结果
        return velocity
```

**优势**:
- 训练更稳定
- 采样步骤更少
- 生成质量更高

---

## 4. 代码结构分析

### 4.1 目录结构

```
Wan2.2/
├── wan/
│   ├── __init__.py
│   ├── model/
│   │   ├── __init__.py
│   │   ├── ae/
│   │   │   ├── autovideo.py       # VAE模型定义
│   │   │   └── ...
│   │   ├── llm/
│   │   │   ├── umt5.py           # 文本编码器
│   │   │   └── ...
│   │   ├── mmt/
│   │   │   ├── dit.py            # DiT主网络
│   │   │   ├── moe.py            # MoE实现
│   │   │   └── ...
│   │   └── utils/
│   │       └── ...
│   ├── pipeline/
│   │   ├── t2v_pipeline.py       # 文本到视频管道
│   │   ├── i2v_pipeline.py        # 图像到视频管道
│   │   └── ...
│   └── ...
├── generate.py                    # 生成入口
├── config/
│   └── ...
└── tests/
    └── ...
```

### 4.2 核心模块

#### 4.2.1 VAE模块 (`wan/model/ae/autovideo.py`)

```python
# 关键组件
class AutoVideoVAE(nn.Module):
    """3D视频VAE，用于视频压缩和解压缩"""
    
    def __init__(self, ...):
        # 编码器
        self.encoder = Encoder3D(...)
        # 解码器  
        self.decoder = Decoder3D(...)
        # 量化层
        self.quant_conv = ...
        self.post_quant_conv = ...
    
    def encode(self, x):
        """视频 -> latent"""
        h = self.encoder(x)
        h = self.quant_conv(h)
        return h
    
    def decode(self, z):
        """latent -> 视频"""
        z = self.post_quant_conv(z)
        return self.decoder(z)
```

#### 4.2.2 文本编码器 (`wan/model/llm/umt5.py`)

```python
class UMT5Encoder(nn.Module):
    """UMT5文本编码器"""
    
    def __init__(self, ...):
        # 使用transformers加载UMT5模型
        self.model = T5EncoderModel.from_pretrained(...)
    
    def forward(self, input_ids, attention_mask):
        # 输出文本embedding
        outputs = self.model(
            input_ids=input_ids,
            attention_mask=attention_mask
        )
        return outputs.last_hidden_state
```

#### 4.2.3 MoE DiT模块 (`wan/model/mmt/dit.py` + `moe.py`)

```python
class MoETransformerBlock(nn.Module):
    """MoE Transformer块"""
    
    def __init__(self, dim, num_experts=2, ...):
        self.self_attn = SelfAttention(dim, ...)
        self.cross_attn = CrossAttention(dim, ...)
        
        # MoE层
        self.moe = MoE(
            dim=dim,
            num_experts=num_experts,  # 2个专家
            expert_capacity=...,
        )
    
    def forward(self, x, context, timestep):
        # 自注意力
        x = self.self_attn(x)
        
        # 交叉注意力 (文本条件)
        x = self.cross_attn(x, context)
        
        # MoE前馈网络
        x = self.moe(x)
        
        return x


class MoE(nn.Module):
    """混合专家实现"""
    
    def __init__(self, dim, num_experts=2, ...):
        # 创建多个专家
        self.experts = nn.ModuleList([
            FeedForward(dim) for _ in range(num_experts)
        ])
        
        # 路由网络 (决定使用哪个专家)
        self.router = nn.Linear(dim, num_experts)
    
    def forward(self, x):
        # 计算专家权重
        logits = self.router(x)
        weights = F.softmax(logits, dim=-1)
        
        # 根据SNR选择专家 (简化版)
        # 实际实现会更复杂，考虑timestep
        ...
```

#### 4.2.4 采样管道

```python
# t2v_pipeline.py 核心流程
class TextToVideoPipeline:
    
    def __init__(self, config):
        self.vae = AutoVideoVAE(...)
        self.text_encoder = UMT5Encoder(...)
        self.transformer = FlowMatchingTransformer(...)
    
    @torch.no_grad()
    def generate(self, prompt, num_frames=81, ...):
        # 1. 文本编码
        text_emb = self.text_encoder(prompt)
        
        # 2. 初始化噪声
        latent = torch.randn(...)
        
        # 3. Flow Matching 采样
        for t in tqdm(timesteps):
            # 预测velocity
            v = self.transformer(latent, t, text_emb)
            
            # 更新latent
            latent = latent + v * dt
        
        # 4. VAE解码
        video = self.vae.decode(latent)
        
        return video
```

---

## 5. 生成流程

### 5.1 完整生成流程图

```
┌──────────────────────────────────────────────────────────────────┐
│                     Text-to-Video 生成流程                        │
└──────────────────────────────────────────────────────────────────┘

Step 1: 输入处理
┌─────────────┐
│ "一只猫在    │ ──► Tokenize ──► [256, 1024, 315, ...]
│  草地上奔跑" │
└─────────────┘
       │
       ▼
Step 2: 文本编码
┌──────────────────────────────────┐
│        UMT5 Encoder              │
│  [256, 1024, 315, ...] ──►       │
│         [batch, 512, 768]        │  (文本embedding)
└──────────────────────────────────┘
       │
       ▼
Step 3: 初始化噪声
┌──────────────────────────────────┐
│   随机噪声 latent (T×H×W)         │
│   81帧 × 45 × 20 latent          │
└──────────────────────────────────┘
       │
       ▼
Step 4: 去噪循环 (27-50步)
┌──────────────────────────────────┐
│  For each timestep:               │
│    1. 计算当前SNR                │
│    2. 选择Expert (High/Low)      │
│    3. Self-Attention             │
│    4. Cross-Attention (文本)     │
│    5. MoE FFN                    │
│    6. 更新latent                 │
└──────────────────────────────────┘
       │
       ▼
Step 5: VAE解码
┌──────────────────────────────────┐
│   Latent ──► VAE Decode ──►     │
│   [B, C, T, H, W] 视频           │
└──────────────────────────────────┘
       │
       ▼
Output: 5秒/81帧/720P视频
```

### 5.2 关键参数

| 参数 | 说明 | 推荐值 |
|------|------|--------|
| `num_frames` | 生成帧数 | 81 (5秒@16fps) |
| `num_inference_steps` | 推理步数 | 27-50 |
| `guidance_scale` | 提示词引导强度 | 3.5-7.0 |
| `shift` | 时间偏移参数 | 5 |
| `resolution` | 输出分辨率 | 720p, 480p |
| `fps` | 帧率 | 16, 24, 30 |

---

## 6. 与其他模型对比

### 6.1 Wan 2.2 vs Wan 2.1

| 特性 | Wan 2.1 | Wan 2.2 |
|------|---------|---------|
| 架构 | Dense | MoE |
| 总参数 | - | 27B |
| 激活参数 | - | 14B |
| 训练数据 | 基准 | +65.6%图像, +83.2%视频 |
| 美学控制 | 基础 | 电影级 |
| 运动质量 | 基准 | 显著提升 |
| 生成时间 | 45-60s | ~9min (单卡) |

### 6.2 与闭源模型对比

| 模型 | 分辨率 | 开放性 | MoE |
|------|--------|--------|-----|
| Wan 2.2 | 720P | 开源 (Apache 2.0) | ✓ |
| OpenAI Sora | 1080P | 闭源 | ✗ |
| Runway Gen-3 | 720P | 闭源 | ✗ |
| Kling 2.0 | 1080P | 闭源 | ✗ |

---

## 7. 部署与使用

### 7.1 硬件要求

| 模型 | 最低显存 | 推荐显存 |
|------|----------|----------|
| TI2V-5B | 8GB | 16GB (RTX 3060 Ti - RTX 4070) |
| T2V-A14B | 16GB | 24GB+ (RTX 4090 / A100) |
| I2V-A14B | 16GB | 24GB+ (RTX 4090 / A100) |

### 7.2 推理时间估算 (RTX 4090)

| 模型 | 分辨率 | 生成时间 |
|------|--------|----------|
| TI2V-5B | 480P | ~4分钟 |
| TI2V-5B | 720P | ~9分钟 |
| T2V-A14B | 720P | ~9分钟 (多卡更快) |

---

## 8. 总结

Wan 2.2 代表了开源视频生成的重要里程碑：

1. **MoE架构创新**: 首次将混合专家机制成功应用于视频扩散模型，实现了质量和效率的平衡
2. **电影级控制**: 专业的美学标签系统支持精细的视觉控制
3. **消费级可用**: 5B版本让更多用户能够在个人设备上体验AI视频生成
4. **完全开源**: Apache 2.0许可证允许商业和非商业使用

对于学习者来说，理解以下核心概念即可掌握Wan 2.2：
- VAE视频压缩
- UMT5文本编码
- Flow Matching采样
- MoE双专家切换机制
- SNR在去噪过程中的作用

---

*文档生成时间: 2026年3月*
*参考资料: Wan 2.2 GitHub, 官方博客, 各种技术分析文章*
