# Wan 2.2 模型结构详解

> 本文档为 Wan 2.2 视频生成模型的结构解析，帮助初学者熟悉模型架构和代码组织。

---

## 1. 模型概述

Wan 2.2 是由阿里巴巴通义实验室开发的开源视频生成模型，于2025年7月发布。主要特性：

- **MoE 架构**：首个采用混合专家机制的 video diffusion 模型
- **高效高清**：支持 720P@24fps 视频生成，可在消费级 GPU（如 RTX 4090）运行
- **多任务支持**：Text-to-Video (T2V)、Image-to-Video (I2V)、Speech-to-Video (S2V)、Character Animation

---

## 2. 核心架构创新

### 2.1 MoE (Mixture-of-Experts) 混合专家架构

Wan 2.2 首次将 MoE 引入视频扩散模型，核心思想：

```
┌─────────────────────────────────────────────────────────────┐
│                    Denoising Process                        │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  High-Noise Expert (14B)  ──┐                              │
│                              ├───> Switch by SNR ──> Output  │
│  Low-Noise Expert (14B)   ──┘                              │
│                                                             │
│  Total Parameters: 27B                                     │
│  Active Parameters per step: 14B                          │
└─────────────────────────────────────────────────────────────┘
```

**专家分工**：
- **High-Noise Expert（高噪声专家）**：处理去噪早期阶段，专注于整体布局、构图、运动模式
- **Low-Noise Expert（低噪声专家）**：处理去噪后期阶段，精炼纹理细节、美学质量、时序一致性

**切换机制**：
- 基于 Signal-to-Noise Ratio (SNR) 自动切换
- SNR 随去噪步数 t 增加而单调递减
- 定义阈值 t_moe，当 t < t_moe 时切换到低噪声专家

### 2.2 高压缩 VAE

TI2V-5B 模型使用 Wan2.2-VAE，实现极高压缩率：

| 维度 | 压缩比 |
|------|--------|
| 时间 T | 4x |
| 高度 H | 16x |
| 宽度 W | 16x |
| **总压缩率** | **64x** (不含 patchify) / **1024x** (含 patchify) |

---

## 3. 模型变体

| 模型 | 参数量 | 功能 | 分辨率 |
|------|--------|------|--------|
| T2V-A14B | 27B (14B active) | Text-to-Video | 480P/720P |
| I2V-A14B | 27B (14B active) | Image-to-Video | 480P/720P |
| TI2V-5B | 5B dense | T2V + I2V | 720P |
| S2V-14B | ~14B | Speech-to-Video | 480P/720P |
| Animate-14B | ~14B | Character Animation | - |

---

## 4. 代码结构分析

```
Wan2.2/
├── generate.py              # 主推理入口
├── wan/                     # 核心代码目录
│   ├── __init__.py
│   ├── modules/             # 模型模块
│   │   ├── __init__.py
│   │   ├── dit/             # DiT (Diffusion Transformer) 核心
│   │   │   ├── __init__.py
│   │   │   ├── model.py     # DiT 模型定义
│   │   │   ├── layer.py     # Transformer 层
│   │   │   ├── mlp.py       # FFN/MLP 模块
│   │   │   └── attention.py # 注意力机制
│   │   ├── vae/             # VAE 视频编码器/解码器
│   │   │   ├── __init__.py
│   │   │   ├── model.py     # VAE 模型
│   │   │   └── utils.py
│   │   ├── t5/              # T5 文本编码器
│   │   ├── embedder/        # 条件 embedding
│   │   │   ├── __init__.py
│   │   │   ├── timestep.py  # 时间步嵌入
│   │   │   └── text.py      # 文本嵌入
│   │   ├── schedulers/      # 扩散调度器
│   │   │   ├── __init__.py
│   │   │   └── ddpm.py      # DDPM/flow matching
│   │   └── animate/          # 角色动画模块
│   │       └── preprocess/  # 预处理
│   ├── inference/           # 推理pipeline
│   │   ├── __init__.py
│   │   ├── pipeline.py      # 主推理类
│   │   └── sampler.py       # 采样器
│   └── utils/               # 工具函数
│       ├── __init__.py
│       └── model_utils.py
├── examples/                 # 示例
├── tests/                    # 测试
└── requirements.txt
```

---

## 5. 核心组件详解

### 5.1 DiT (Diffusion Transformer)

DiT 是 Wan 2.2 的核心扩散模型，基于 Transformer 架构：

```python
# wan/modules/dit/model.py 主要结构
class WanDiT(nn.Module):
    def __init__(self, ...):
        # Patchify: 将视频转换为 patches
        self.patchify = Patchify(...)
        
        # Transformer blocks
        self.blocks = nn.ModuleList([
            DiTBlock(hidden_size, num_heads)
            for _ in range(depth)
        ])
        
        # 输出层
        self.final_layer = FinalLayer(hidden_size, ...)
    
    def forward(self, x, timestep, condition):
        # 1. Patchify
        x = self.patchify(x)
        
        # 2. 添加条件嵌入
        x = x + self.timestep_embed(timestep)
        x = x + self.condition_embed(condition)
        
        # 3. Transformer blocks
        for block in self.blocks:
            x = block(x)
        
        # 4. 重建输出
        output = self.final_layer(x)
        return output
```

### 5.2 MoE 实现

```python
# wan/modules/dit/model.py - MoE 核心逻辑
class MoEDiT(nn.Module):
    def __init__(self, ...):
        # 两个专家模型
        self.high_noise_expert = DiTModel(...)
        self.low_noise_expert = DiTModel(...)
        
    def forward(self, x, timestep, condition, guidance):
        # 计算 SNR
        snr = self.compute_snr(timestep)
        
        # 根据 SNR 阈值选择专家
        if snr > self.snr_threshold:
            # 高噪声阶段：使用高噪声专家
            output = self.high_noise_expert(x, timestep, condition, guidance)
        else:
            # 低噪声阶段：使用低噪声专家
            output = self.low_noise_expert(x, timestep, condition, guidance)
        
        return output
```

### 5.3 VAE 编码器/解码器

```python
# wan/modules/vae/model.py
class WanVAE(nn.Module):
    def __init__(self, ...):
        # 编码器
        self.encoder = Encoder(
            in_channels=3,
            out_channels=16,  # 潜在空间通道数
            latent_channels=16,
            compression_ratio="4x16x16"  # T×H×W 压缩
        )
        
        # 解码器
        self.decoder = Decoder(
            in_channels=16,
            out_channels=3,
            latent_channels=16
        )
    
    def encode(self, video):
        # 视频 -> 潜在表示
        return self.encoder(video)
    
    def decode(self, latent):
        # 潜在表示 -> 视频
        return self.decoder(latent)
```

### 5.4 推理 Pipeline

```python
# wan/inference/pipeline.py
class WanPipeline:
    def __init__(self, config):
        # 加载各组件
        self.vae = load_vae(config.vae_path)
        self.dit = load_dit(config.dit_path)  # 可能包含 MoE
        self.text_encoder = load_t5(config.t5_path)
        self.scheduler = DDIMScheduler()
    
    @torch.no_grad()
    def generate(self, prompt, image=None, num_steps=50):
        # 1. 编码文本
        text_embeddings = self.text_encoder(prompt)
        
        # 2. 初始化噪声
        latents = self.prepare_latents(...)
        
        # 3. 扩散去噪
        for t in tqdm(self.scheduler.timesteps):
            # 预测噪声
            noise_pred = self.dit(latents, t, text_embeddings)
            
            # 更新 latents
            latents = self.scheduler.step(noise_pred, t, latents)
        
        # 4. VAE 解码
        video = self.vae.decode(latents)
        
        return video
```

---

## 6. 推理流程图

```
┌──────────────────────────────────────────────────────────────────┐
│                        Input                                      │
│    ┌─────────────┐          ┌──────────────┐                    │
│    │ Text Prompt │          │  Source Image│ (可选)             │
│    └──────┬──────┘          └──────┬───────┘                    │
│           │                       │                              │
│           ▼                       ▼                              │
│    ┌─────────────┐          ┌──────────────┐                    │
│    │T5 Encoder  │          │   VAE Encode  │                    │
│    └──────┬──────┘          └──────┬───────┘                    │
│           │                       │                              │
│           └───────────┬────────────┘                            │
│                       ▼                                          │
│              ┌─────────────────┐                                  │
│              │  Concatenate    │                                  │
│              │  as Condition  │                                  │
│              └────────┬────────┘                                  │
│                       │                                          │
│                       ▼                                          │
│              ┌─────────────────┐                                  │
│              │  Init Gaussian  │                                  │
│              │     Noise      │                                  │
│              └────────┬────────┘                                  │
│                       │                                          │
│           ┌──────────┴──────────┐                                │
│           │                     │                                │
│           ▼                     ▼                                │
│    ┌─────────────┐      ┌──────────────┐                        │
│    │High-Noise   │      │ Low-Noise    │  ← MoE Switch         │
│    │ Expert      │      │ Expert       │                        │
│    │ (t > t_moe)│      │ (t <= t_moe) │                        │
│    └──────┬──────┘      └──────┬───────┘                        │
│           │                     │                                │
│           └──────────┬──────────┘                                │
│                      ▼                                           │
│              ┌─────────────────┐                                  │
│              │    Scheduler    │  ← 迭代去噪                     │
│              │    Step Update   │                                  │
│              └────────┬────────┘                                  │
│                       │                                          │
│                       └───────────┬                               │
│                                   ▼                               │
│                          ┌─────────────┐                          │
│                          │VAE Decode   │                          │
│                          └──────┬──────┘                          │
│                                 │                                 │
│                                 ▼                                 │
│                          ┌─────────────┐                          │
│                          │Output Video │                          │
│                          └─────────────┘                          │
└──────────────────────────────────────────────────────────────────┘
```

---

## 7. 关键配置参数

推理时常用参数：

```bash
# 基础参数
--task t2v-A14B              # 任务类型
--size 1280x720              # 输出分辨率
--ckpt_dir ./Wan2.2-T2V-A14B # 模型路径
--prompt "your prompt"       # 文本提示
--offload_model True         # 模型卸载（省显存）
--convert_model_dtype        # 转换数据类型

# 高级参数
--dit_fsdp                   # DiT 分布式
--t5_fsdp                    # T5 分布式
--ulysses_size 8             # Ulysses 并行
--use_prompt_extend          # Prompt 扩展
--num_inference_steps 50     # 去噪步数
--guidance_scale 7.5         # CFG 强度
```

---

## 8. 显存需求

| 模型 | 单卡 | 8卡 |
|------|------|-----|
| T2V-A14B (MoE) | 80GB+ | 8x80GB |
| I2V-A14B (MoE) | 80GB+ | 8x80GB |
| TI2V-5B | 24GB (RTX 4090) | - |
| S2V-14B | 80GB+ | 8x80GB |

---

## 9. 相关论文

- [Wan: Open and Advanced Large-Scale Video Generative Models](https://arxiv.org/abs/2503.20314) (arXiv:2503.20314)

---

## 10. 延伸学习资源

- GitHub: https://github.com/Wan-Video/Wan2.2
- HuggingFace: https://huggingface.co/Wan-AI/
- 官方Demo: https://wan.video/
- Discord: https://discord.gg/AKNgpMK4Yj

---

*文档生成时间: 2025年*
