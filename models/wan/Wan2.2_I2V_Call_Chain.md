# Wan 2.2 I2V (Image-to-Video) 模型完整调用链详解

本文档详细解释从运行 `generate.py` 开始到最终生成视频的完整函数调用链。

---

## 目录

1. [入口函数: main()](#1-main-入口函数)
2. [参数解析与验证](#2-参数解析与验证)
3. [模型初始化](#3-模型初始化)
4. [图像预处理](#4-图像预处理)
5. [文本编码](#5-文本编码)
6. [VAE编码图像](#6-vae编码图像)
7. [噪声初始化与Mask生成](#7-噪声初始化与mask生成)
8. [Flow Matching 采样循环](#8-flow-matching-采样循环)
9. [专家模型切换机制](#9-专家模型切换机制)
10. [模型前向传播 (WanModel)](#10-模型前向传播-wanmodel)
11. [注意力计算](#11-注意力计算)
12. [VAE解码](#12-vae解码)
13. [视频保存](#13-视频保存)

---

## 1. main() 入口函数

**文件**: `generate.py`, 行 574-575

```python
if __name__ == "__main__":
    args = _parse_args()  # 解析命令行参数
    generate(args)        # 调用生成函数
```

### 1.1 _parse_args() - 参数解析

**文件**: `generate.py`, 行 105-300

```python
def _parse_args():
    parser = argparse.ArgumentParser(...)
    
    # 添加各种命令行参数
    parser.add_argument("--task", type=str, default="t2v-A14B")  # 任务类型
    parser.add_argument("--size", type=str, default="1280*720")   # 视频尺寸
    parser.add_argument("--frame_num", type=int, default=None)     # 帧数
    parser.add_argument("--ckpt_dir", type=str, default=None)      # 模型路径
    parser.add_argument("--prompt", type=str, default=None)        # 文本提示
    parser.add_argument("--image", type=str, default=None)          # 输入图像
    parser.add_argument("--sample_solver", type=str, default='unipc')
    parser.add_argument("--sample_steps", type=int, default=None)
    parser.add_argument("--sample_shift", type=float, default=None)
    parser.add_argument("--sample_guide_scale", type=float, default=None)
    parser.add_argument("--offload_model", type=str2bool, default=None)
    # ... 其他参数
    
    args = parser.parse_args()
    _validate_args(args)  # 验证参数
    return args
```

**参数验证** (`_validate_args`):
- 检查 `--ckpt_dir` 是否指定
- 检查 `--task` 是否在 `WAN_CONFIGS` 中
- 检查 `--image` 是否为 I2V 任务指定

---

## 2. 参数解析与验证

### 2.1 _validate_args() - 参数验证

**文件**: `generate.py`, 行 62-103

```python
def _validate_args(args):
    # 1. 基本检查
    assert args.ckpt_dir is not None, "Please specify the checkpoint directory."
    assert args.task in WAN_CONFIGS, f"Unsupport task: {args.task}"
    
    # 2. 如果没有提供prompt，使用示例prompt
    if args.prompt is None:
        args.prompt = EXAMPLE_PROMPT[args.task]["prompt"]
    
    # 3. 如果没有提供image，使用示例image
    if args.image is None and "image" in EXAMPLE_PROMPT[args.task]:
        args.image = EXAMPLE_PROMPT[args.task]["image"]
    
    # 4. I2V任务必须指定image
    if args.task == "i2v-A14B":
        assert args.image is not None, "Please specify the image path for i2v."
    
    # 5. 从配置中获取默认参数
    cfg = WAN_CONFIGS[args.task]
    if args.sample_steps is None:
        args.sample_steps = cfg.sample_steps
    if args.sample_shift is None:
        args.sample_shift = cfg.sample_shift
    if args.sample_guide_scale is None:
        args.sample_guide_scale = cfg.sample_guide_scale
    if args.frame_num is None:
        args.frame_num = cfg.frame_num
```

### 2.2 WAN_CONFIGS 配置加载

**文件**: `wan/configs/__init__.py`

```python
# I2V-A14B 配置 (wan/configs/wan_i2v_A14B.py)
i2v_A14B = EasyDict(__name__='Config: Wan I2V A14B')
i2v_A14B.update(wan_shared_cfg)

# T5 编码器
i2v_A14B.t5_checkpoint = 'models_t5_umt5-xxl-enc-bf16.pth'
i2v_A14B.t5_tokenizer = 'google/umt5-xxl'

# VAE
i2v_A14B.vae_checkpoint = 'Wan2.1_VAE.pth'
i2v_A14B.vae_stride = (4, 8, 8)  # 时间4x, 空间8x

# DiT Transformer
i2v_A14B.patch_size = (1, 2, 2)
i2v_A14B.dim = 5120
i2v_A14B.ffn_dim = 13824
i2v_A14B.num_heads = 40
i2v_A14B.num_layers = 40
i2v_A14B.low_noise_checkpoint = 'low_noise_model'
i2v_A14B.high_noise_checkpoint = 'high_noise_model'

# 推理参数
i2v_A14B.sample_shift = 5.0
i2v_A14B.sample_steps = 40
i2v_A14B.boundary = 0.900  # 专家切换边界
i2v_A14B.sample_guide_scale = (3.5, 3.5)
```

---

## 3. 模型初始化

### 3.1 generate() - 主生成函数

**文件**: `generate.py`, 行 315-340

```python
def generate(args):
    rank = int(os.getenv("RANK", 0))
    world_size = int(os.getenv("WORLD_SIZE", 1))
    local_rank = int(os.getenv("LOCAL_RANK", 0))
    device = local_rank
    
    # 初始化分布式环境 (如果需要)
    if world_size > 1:
        dist.init_process_group(...)
    
    # 加载配置
    cfg = WAN_CONFIGS[args.task]
```

### 3.2 加载输入图像

**文件**: `generate.py`, 行 374-377

```python
img = None
if args.image is not None:
    img = Image.open(args.image).convert("RGB")  # 打开并转换图像
    logging.info(f"Input image: {args.image}")
```

### 3.3 创建 WanI2V Pipeline

**文件**: `generate.py`, 行 517-528

```python
else:  # i2v-A14B
    logging.info("Creating WanI2V pipeline.")
    wan_i2v = wan.WanI2V(
        config=cfg,
        checkpoint_dir=args.ckpt_dir,
        device_id=device,
        rank=rank,
        t5_fsdp=args.t5_fsdp,
        dit_fsdp=args.dit_fsdp,
        use_sp=(args.ulysses_size > 1),
        t5_cpu=args.t5_cpu,
        convert_model_dtype=args.convert_model_dtype,
    )
```

### 3.4 WanI2V.__init__() - 模型组件初始化

**文件**: `wan/image2video.py`, 行 35-126

```python
class WanI2V:
    def __init__(self, config, checkpoint_dir, device_id=0, ...):
        self.device = torch.device(f"cuda:{device_id}")
        self.config = config
        self.num_train_timesteps = config.num_train_timesteps  # 1000
        self.boundary = config.boundary  # 0.900
        self.param_dtype = config.param_dtype  # bfloat16
        
        # ========== 1. 初始化 T5 文本编码器 ==========
        shard_fn = partial(shard_model, device_id=device_id)
        self.text_encoder = T5EncoderModel(
            text_len=config.text_len,           # 512
            dtype=config.t5_dtype,              # bfloat16
            device=torch.device('cpu'),
            checkpoint_path=os.path.join(checkpoint_dir, config.t5_checkpoint),
            tokenizer_path=os.path.join(checkpoint_dir, config.t5_tokenizer),
            shard_fn=shard_fn if t5_fsdp else None,
        )
        
        # ========== 2. 初始化 VAE ==========
        self.vae_stride = config.vae_stride    # (4, 8, 8)
        self.patch_size = config.patch_size     # (1, 2, 2)
        self.vae = Wan2_1_VAE(
            vae_pth=os.path.join(checkpoint_dir, config.vae_checkpoint),
            device=self.device
        )
        
        # ========== 3. 初始化低噪声专家模型 ==========
        self.low_noise_model = WanModel.from_pretrained(
            checkpoint_dir, subfolder=config.low_noise_checkpoint)
        self.low_noise_model = self._configure_model(
            model=self.low_noise_model, ...)
        
        # ========== 4. 初始化高噪声专家模型 ==========
        self.high_noise_model = WanModel.from_pretrained(
            checkpoint_dir, subfolder=config.high_noise_checkpoint)
        self.high_noise_model = self._configure_model(
            model=self.high_noise_model, ...)
```

#### 3.4.1 T5EncoderModel 初始化

**文件**: `wan/modules/t5.py`, 行 472-512

```python
class T5EncoderModel:
    def __init__(self, text_len, dtype, device, checkpoint_path, tokenizer_path, ...):
        self.text_len = text_len
        self.dtype = dtype
        self.device = device
        
        # 1. 加载 UMT5-XXL 编码器模型
        model = umt5_xxl(
            encoder_only=True,
            return_tokenizer=False,
            dtype=dtype,
            device=device
        ).eval().requires_grad_(False)
        
        # 2. 加载预训练权重
        model.load_state_dict(torch.load(checkpoint_path, map_location='cpu'))
        
        # 3. 分片 (如果使用FSDP)
        if shard_fn is not None:
            self.model = shard_fn(self.model, sync_module_states=False)
        else:
            self.model.to(self.device)
        
        # 4. 初始化分词器
        self.tokenizer = HuggingfaceTokenizer(
            name=tokenizer_path,  # 'google/umt5-xxl'
            seq_len=text_len,     # 512
            clean='whitespace'
        )
```

**Umt5-xxl 配置**:
- `vocab_size`: 256384
- `dim`: 4096
- `dim_attn`: 4096
- `dim_ffn`: 10240
- `num_heads`: 64
- `encoder_layers`: 24

#### 3.4.2 Wan2_1_VAE 初始化

**文件**: `wan/modules/vae2_1.py`

```python
class Wan2_1_VAE:
    def __init__(self, vae_pth, device, ...):
        # 加载预训练的VAE模型
        self.model = _video_vae(
            pretrained_path=vae_pth,
            z_dim=16,
            dim=160,
            dim_mult=[1, 2, 4, 4],
            temperal_downsample=[True, True, True],
        ).eval().requires_grad_(False).to(device)
```

#### 3.4.3 WanModel (DiT) 初始化

**文件**: `wan/modules/model.py`

```python
class WanModel(ModelMixin, ConfigMixin):
    @register_to_config
    def __init__(self,
                 model_type='i2v',      # I2V模式
                 patch_size=(1,2,2),
                 text_len=512,
                 in_dim=16,
                 dim=5120,              # A14B
                 ffn_dim=13824,
                 num_heads=40,
                 num_layers=40,
                 ...):
        
        # 1. Patch Embedding - 将视频转为序列
        self.patch_embedding = nn.Conv3d(in_dim, dim, kernel_size=patch_size, stride=patch_size)
        
        # 2. Text Embedding - 文本条件投影
        self.text_embedding = nn.Sequential(
            nn.Linear(text_dim, dim), nn.GELU(), nn.Linear(dim, dim)
        )
        
        # 3. Time Embedding - 时间步嵌入
        self.time_embedding = nn.Sequential(
            nn.Linear(freq_dim, dim), nn.SiLU(), nn.Linear(dim, dim)
        )
        self.time_projection = nn.Sequential(nn.SiLU(), nn.Linear(dim, dim * 6))
        
        # 4. Transformer Blocks (40层)
        self.blocks = nn.ModuleList([
            WanAttentionBlock(dim, ffn_dim, num_heads, ...) 
            for _ in range(num_layers)
        ])
        
        # 5. Output Head
        self.head = Head(dim, out_dim, patch_size)
        
        # 6. RoPE 频率缓存
        self.freqs = torch.cat([
            rope_params(1024, d - 4 * (d // 6)),
            rope_params(1024, 2 * (d // 6)),
            rope_params(1024, 2 * (d // 6))
        ], dim=1)
```

---

## 4. 图像预处理

### 4.1 WanI2V.generate() - 图像预处理

**文件**: `wan/image2video.py`, 行 256-272

```python
def generate(self, input_prompt, img, max_area=720*1280, frame_num=81, ...):
    # ========== 步骤1: 图像预处理 ==========
    # 将PIL图像转换为tensor并归一化到[-1, 1]
    # TF.to_tensor: [0,255] -> [0,1]
    # .sub_(0.5).div_(0.5): [0,1] -> [-1,1]
    img = TF.to_tensor(img).sub_(0.5).div_(0.5).to(self.device)
    # img shape: [3, H, W]
    
    F = frame_num  # 81帧
    h, w = img.shape[1:]  # 原始图像尺寸
    
    # ========== 步骤2: 根据max_area计算目标尺寸 ==========
    aspect_ratio = h / w
    # 计算latent空间尺寸
    lat_h = round(
        np.sqrt(max_area * aspect_ratio) // self.vae_stride[1] //
        self.patch_size[1] * self.patch_size[1])
    lat_w = round(
        np.sqrt(max_area / aspect_ratio) // self.vae_stride[2] //
        self.patch_size[2] * self.patch_size[2])
    
    # 反推像素尺寸
    h = lat_h * self.vae_stride[1]  # 高度
    w = lat_w * self.vae_stride[2]  # 宽度
    
    # 假设 max_area=720*1280, aspect_ratio=0.5625 (9:16)
    # lat_h ≈ 45, lat_w ≈ 80
    # h = 360, w = 640
```

---

## 5. 文本编码

### 5.1 T5EncoderModel.__call__()

**文件**: `wan/modules/t5.py`, 行 506-512

```python
def __call__(self, texts, device):
    # 输入: texts = ["a cat sitting on a beach..."]
    # device = cuda:0
    
    # ========== 步骤1: Tokenize ==========
    # 调用 HuggingfaceTokenizer
    ids, mask = self.tokenizer(
        texts,                    # 文本列表
        return_mask=True,         # 返回attention mask
        add_special_tokens=True  # 添加特殊token
    )
    # ids: [1, seq_len] (例如 [1, 128])
    # mask: [1, seq_len]
    
    ids = ids.to(device)
    mask = mask.to(device)
    
    # ========== 步骤2: 计算实际序列长度 ==========
    seq_lens = mask.gt(0).sum(dim=1).long()
    # seq_lens: [实际token数]
    
    # ========== 步骤3: T5编码 ==========
    # self.model = T5Encoder
    context = self.model(ids, mask)
    # context: [1, actual_seq_len, 4096]
    
    # ========== 步骤4: 截取实际长度 ==========
    return [u[:v] for u, v in zip(context, seq_lens)]
    # 返回: [tensor[actual_seq_len, 4096]]
```

#### 5.1.1 HuggingfaceTokenizer.__call__()

**文件**: `wan/modules/tokenizers.py`, 行 49-73

```python
def __call__(self, sequence, **kwargs):
    return_mask = kwargs.pop('return_mask', False)
    
    # 构建tokenizer参数
    _kwargs = {'return_tensors': 'pt'}
    if self.seq_len is not None:
        _kwargs.update({
            'padding': 'max_length',
            'truncation': True,
            'max_length': self.seq_len  # 512
        })
    
    # Tokenize
    if isinstance(sequence, str):
        sequence = [sequence]
    if self.clean:
        sequence = [self._clean(u) for u in sequence]
    
    ids = self.tokenizer(sequence, **_kwargs)
    # ids.input_ids: [1, 512]
    # ids.attention_mask: [1, 512]
    
    if return_mask:
        return ids.input_ids, ids.attention_mask
    else:
        return ids.input_ids
```

---

## 6. VAE编码图像

### 6.1 WanI2V.generate() - VAE编码

**文件**: `wan/image2video.py`, 行 314-323

```python
# ========== 步骤1: 图像预处理 ==========
# 将第一帧图像resize到目标尺寸，然后与空白帧拼接
y = self.vae.encode([
    torch.concat([
        # 图像帧: 插值到目标尺寸
        torch.nn.functional.interpolate(
            img[None].cpu(),   # [1, 3, H, W]
            size=(h, w),       # 目标尺寸
            mode='bicubic'
        ).transpose(0, 1),   # [1, 3, H, W] -> [3, 1, H, W]
        
        # 后续帧: 零填充 (F-1帧)
        torch.zeros(3, F - 1, h, w)
    ],
    dim=1  # 在通道维度拼接
).to(self.device)
# y shape: [C, F', H', W']
# 其中 C = latent通道数, F' = 压缩后帧数
```

### 6.2 Wan2_1_VAE.encode()

**文件**: `wan/modules/vae2_1.py`

```python
def encode(self, videos):
    """
    视频 -> Latent
    videos: list of [C, F, H, W]
    """
    x = patchify(x, patch_size=2)  # 2x空间压缩
    
    # 分块编码 (处理长视频)
    t = x.shape[2]
    iter_ = 1 + (t - 1) // 4
    
    for i in range(iter_):
        if i == 0:
            out = self.encoder(x[:, :, :1, :, :], ...)
        else:
            out_ = self.encoder(x[:, :, 1+4*(i-1):1+4*i, :, :], ...)
            out = torch.cat([out, out_], 2)
    
    mu, log_var = self.conv1(out).chunk(2, dim=1)
    
    # 归一化
    mu = (mu - scale[0]) * scale[1]
    
    return mu  # [C, F', H', W']
```

### 6.3 构建Mask

**文件**: `wan/image2video.py`, 行 289-296

```python
# ========== 构建条件Mask ==========
# 第一帧保持内容，其他帧由模型生成
msk = torch.ones(1, F, lat_h, lat_w, device=self.device)
msk[:, 1:] = 0  # 第一帧之后都是0 (需要生成)

# 扩展维度
msk = torch.concat([
    torch.repeat_interleave(msk[:, 0:1], repeats=4, dim=1),  # 第一帧重复4次
    msk[:, 1:]  # 后续帧
], dim=1)
# msk: [1, F*4, lat_h, lat_w]

# 调整形状用于拼接
msk = msk.view(1, msk.shape[1] // 4, 4, lat_h, lat_w)
msk = msk.transpose(1, 2)[0]  # [F, 4, lat_h, lat_w]

# 最终拼接: [C+1, F, lat_h, lat_w]
# C个通道的latent + 1个mask通道
y = torch.concat([msk, y])
# y shape: [16+1=17, F', lat_h, lat_w]
```

---

## 7. 噪声初始化与Mask生成

### 7.1 初始化随机噪声

**文件**: `wan/image2video.py`, 行 277-287

```python
# ========== 初始化噪声 ==========
seed = seed if seed >= 0 else random.randint(0, sys.maxsize)
seed_g = torch.Generator(device=self.device)
seed_g.manual_seed(seed)

# 生成随机噪声
# 注意: 只对后续帧加噪，第一帧由输入图像控制
noise = torch.randn(
    16,                          # latent通道数 (z_dim)
    (F - 1) // self.vae_stride[0] + 1,  # 时间维度
    lat_h,                       # 空间高度 (latent)
    lat_w,                       # 空间宽度 (latent)
    dtype=torch.float32,
    generator=seed_g,
    device=self.device
)
# noise shape: [16, 20, lat_h, lat_w]
# 第一帧位置保留为0 (由图像控制)
```

### 7.2 推理参数设置

**文件**: `wan/image2video.py`, 行 273-275

```python
# 计算最大序列长度
max_seq_len = ((F - 1) // self.vae_stride[0] + 1) * lat_h * lat_w // (
    self.patch_size[1] * self.patch_size[2])
max_seq_len = int(math.ceil(max_seq_len / self.sp_size)) * self.sp_size
```

---

## 8. Flow Matching 采样循环

### 8.1 初始化采样器

**文件**: `wan/image2video.py`, 行 341-362

```python
# ========== 初始化采样调度器 ==========
boundary = self.boundary * self.num_train_timesteps
# boundary = 0.900 * 1000 = 900

if sample_solver == 'unipc':
    # UniPC 采样器 (更快更好)
    sample_scheduler = FlowUniPCMultistepScheduler(
        num_train_timesteps=self.num_train_timesteps,  # 1000
        shift=1,
        use_dynamic_shifting=False
    )
    sample_scheduler.set_timesteps(
        sampling_steps,    # 40步
        device=self.device,
        shift=shift       # 5.0
    )
    timesteps = sample_scheduler.timesteps
else:
    # DPM++ 采样器
    sample_scheduler = FlowDPMSolverMultistepScheduler(...)
    sampling_sigmas = get_sampling_sigmas(sampling_steps, shift)
    timesteps, _ = retrieve_timesteps(...)
```

#### 8.1.1 FlowUniPCMultistepScheduler.set_timesteps()

**文件**: `wan/utils/fm_solvers_unipc.py`, 行 162-240

```python
def set_timesteps(self, num_inference_steps, device, shift=1.0):
    # 1. 生成线性sigma序列
    sigmas = np.linspace(self.sigma_max, self.sigma_min, num_inference_steps + 1)[:-1]
    # sigma: [1.0, 0.975, 0.95, ..., 0.0]
    
    # 2. 应用shift调整
    # sigmas = shift * sigmas / (1 + (shift-1) * sigmas)
    # shift=5.0: 使噪声调度更集中在高噪声阶段
    sigmas = shift * sigmas / (1 + (shift - 1) * sigmas)
    
    # 3. 转换为timestep
    self.sigmas = torch.tensor(sigmas).to(device)
    self.timesteps = sigmas * num_train_timesteps
    # timesteps: [1000, 975, 950, ..., 25]
```

### 8.2 去噪主循环

**文件**: `wan/image2video.py`, 行 382-414

```python
# ========== 主去噪循环 (40步) ==========
latent = noise  # 初始噪声

# 构建条件参数
arg_c = {
    'context': [context[0]],    # 文本embedding
    'seq_len': max_seq_len,
    'y': [y],                   # 图像latent + mask
}

arg_null = {
    'context': context_null,    # 负向提示embedding
    'seq_len': max_seq_len,
    'y': [y],
}

for _, t in enumerate(tqdm(timesteps)):
    # ========== 步骤1: 准备输入 ==========
    latent_model_input = [latent.to(self.device)]
    timestep = torch.stack([t]).to(self.device)
    
    # ========== 步骤2: 选择专家模型 ==========
    model = self._prepare_model_for_timestep(t, boundary, offload_model)
    
    # ========== 步骤3: 选择引导强度 ==========
    sample_guide_scale = guide_scale[1] if t.item() >= boundary else guide_scale[0]
    # t >= 900: high_noise专家, scale=3.5
    # t < 900: low_noise专家, scale=3.5
    
    # ========== 步骤4: 条件预测 (有文本) ==========
    noise_pred_cond = model(
        latent_model_input, 
        t=timestep, 
        **arg_c
    )[0]
    
    # ========== 步骤5: 无条件预测 (负文本) ==========
    noise_pred_uncond = model(
        latent_model_input, 
        t=timestep, 
        **arg_null
    )[0]
    
    # ========== 步骤6: Classifier-Free Guidance ==========
    noise_pred = noise_pred_uncond + sample_guide_scale * (
        noise_pred_cond - noise_pred_uncond
    )
    # CFG: 增强文本控制力
    
    # ========== 步骤7: 采样器更新 ==========
    temp_x0 = sample_scheduler.step(
        noise_pred.unsqueeze(0),  # 预测的噪声
        t,                        # 当前时间步
        latent.unsqueeze(0),     # 当前latent
        return_dict=False,
        generator=seed_g
    )[0]
    
    latent = temp_x0.squeeze(0)
    # 更新latent
```

#### 8.2.1 FlowUniPCMultistepScheduler.step()

**文件**: `wan/utils/fm_solvers_unipc.py` (约行 300+)

```python
def step(self, model_output, timestep, sample, ...):
    """
    Flow Matching 采样步骤
    
    model_output: 预测的velocity (速度场)
    timestep: 当前时间步
    sample: 当前的latent x_t
    """
    # Flow Matching 的核心:
    # x_t = (1-t)*x_0 + t*noise
    # v = dx/dt = noise - x_0
    # 
    # 逆过程: x_{t-dt} = x_t - v * dt
    
    # 计算dt
    step_index = self.step_index
    sigma = self.sigmas[step_index]
    
    # 预测 x0 (从velocity反推)
    # x_0 = x_t - v * t
    pred_original_sample = sample - model_output * sigma
    
    # 计算前一步
    # prev_sample = sample - model_output * (sigma - prev_sigma)
    ...
    
    return prev_sample
```

---

## 9. 专家模型切换机制

### 9.1 _prepare_model_for_timestep()

**文件**: `wan/image2video.py`, 行 172-204

```python
def _prepare_model_for_timestep(self, t, boundary, offload_model):
    """
    根据时间步选择使用哪个专家模型
    
    t: 当前时间步 (例如 950, 850, 100...)
    boundary: 900 (0.9 * 1000)
    """
    
    if t.item() >= boundary:
        # ========== 高噪声阶段 (早期) ==========
        # t ∈ [1000, 900]
        # 使用 high_noise_model
        # 负责: 构图、布局、运动规划
        required_model_name = 'high_noise_model'
        offload_model_name = 'low_noise_model'
    else:
        # ========== 低噪声阶段 (后期) ==========
        # t ∈ [875, 0]
        # 使用 low_noise_model
        # 负责: 细节、纹理、光照
        required_model_name = 'low_noise_model'
        offload_model_name = 'high_noise_model'
    
    # 模型卸载/加载 (节省显存)
    if offload_model or self.init_on_cpu:
        # 将不用的模型移到CPU
        if next(getattr(self, offload_model_name).parameters()).device.type == 'cuda':
            getattr(self, offload_model_name).to('cpu')
        
        # 将要用的模型移到GPU
        if next(getattr(self, required_model_name).parameters()).device.type == 'cpu':
            getattr(self, required_model_name).to(self.device)
    
    return getattr(self, required_model_name)
```

---

## 10. 模型前向传播 (WanModel)

### 10.1 WanModel.forward()

**文件**: `wan/modules/model.py`, 行 410-497

```python
def forward(self, x, t, context, seq_len, y=None):
    """
    I2V模型前向传播
    
    x: list of latent tensor, e.g. [tensor[16, 20, 45, 80]]
    t: timesteps, e.g. tensor[950]
    context: text embedding, e.g. [tensor[128, 4096]]
    seq_len: max sequence length
    y: image latent + mask, e.g. [tensor[17, 20, 45, 80]]
    """
    
    # ========== 步骤1: 处理图像条件 y ==========
    if y is not None:
        # 拼接噪声和图像latent
        x = [torch.cat([u, v], dim=0) for u, v in zip(x, y)]
        # x: [tensor[16+17=33, 20, 45, 80]]
    
    # ========== 步骤2: Patch Embedding ==========
    # 将latent转换为patch序列
    x = [self.patch_embedding(u.unsqueeze(0)) for u in x]
    # patch_embedding: Conv3d(33, 5120, kernel_size=(1,2,2), stride=(1,2,2))
    # 输出: [1, 5120, 20, 45, 80] -> [1, 5120, 20, 22, 40]
    
    # 记录grid尺寸 (用于unpatchify)
    grid_sizes = torch.stack([
        torch.tensor(u.shape[2:], dtype=torch.long) for u in x
    ])
    # grid_sizes: tensor[[20, 22, 40]]
    
    # ========== 步骤3: Flatten + Transpose ==========
    x = [u.flatten(2).transpose(1, 2) for u in x]
    # x: [1, 20*22*40=17600, 5120]
    
    # 序列长度
    seq_lens = torch.tensor([u.size(1) for u in x], dtype=torch.long)
    
    # Padding到统一长度
    x = torch.cat([
        torch.cat([u, u.new_zeros(1, seq_len - u.size(1), u.size(2))], dim=1)
        for u in x
    ])
    # x: [1, max_seq_len, 5120]
    
    # ========== 步骤4: 时间步嵌入 ==========
    if t.dim() == 1:
        t = t.expand(t.size(0), seq_len)  # [1, max_seq_len]
    
    # 正弦时间嵌入
    bt = t.size(0)
    t_flat = t.flatten()
    e = self.time_embedding(
        sinusoidal_embedding_1d(self.freq_dim, t_flat).unflatten(0, (bt, seq_len)).float()
    )
    # e: [1, max_seq_len, 5120]
    
    # 调制参数
    e0 = self.time_projection(e).unflatten(2, (6, self.dim))
    # e0: [1, max_seq_len, 6, 5120]
    
    # ========== 步骤5: 文本条件嵌入 ==========
    context_lens = None
    context = self.text_embedding(
        torch.stack([
            torch.cat([u, u.new_zeros(self.text_len - u.size(0), u.size(1))])
            for u in context
        ])
    )
    # context: [1, 512, 5120]
    
    # ========== 步骤6: 通过Transformer Blocks ==========
    kwargs = dict(
        e=e0,
        seq_lens=seq_lens,
        grid_sizes=grid_sizes,
        freqs=self.freqs,
        context=context,
        context_lens=context_lens
    )
    
    for block in self.blocks:
        x = block(x, **kwargs)
    # x: [1, max_seq_len, 5120]
    
    # ========== 步骤7: 输出头 ==========
    x = self.head(x, e)
    # head: Linear(5120, 16*1*2*2) = 128
    # 输出: [1, max_seq_len, 128]
    
    # ========== 步骤8: Unpatchify ==========
    x = self.unpatchify(x, grid_sizes)
    # 恢复为: [16, 20, 22, 40]
    
    return [u.float() for u in x]
```

### 10.2 WanAttentionBlock.forward()

**文件**: `wan/modules/model.py`, 行 219-259

```python
def forward(self, x, e, seq_lens, grid_sizes, freqs, context, context_lens):
    """
    注意力块前向传播
    """
    # ========== 步骤1: 计算AdaLN调制因子 ==========
    # e: [B, L, 6, C]
    # self.modulation: [1, 6, C]
    e = (self.modulation.unsqueeze(0) + e).chunk(6, dim=2)
    # e: tuple of 6个 [B, L, 1, C]
    
    # ========== 步骤2: 自注意力 (Self-Attention) ==========
    # Pre-LN + AdaLN
    # norm(x) * (1 + shift_scale) + shift_bias
    y = self.self_attn(
        self.norm1(x).float() * (1 + e[1].squeeze(2)) + e[0].squeeze(2),
        seq_lens, grid_sizes, freqs
    )
    x = x + y * e[2].squeeze(2)  # 残差连接 + 门控
    
    # ========== 步骤3: 交叉注意力 (Cross-Attention) + FFN ==========
    def cross_attn_ffn(x, context, context_lens, e):
        # 交叉注意力: 查询来自x, 键值来自文本
        x = x + self.cross_attn(self.norm3(x), context, context_lens)
        
        # FFN: Gated MLP
        y = self.ffn(
            self.norm2(x).float() * (1 + e[4].squeeze(2)) + e[3].squeeze(2)
        )
        x = x + y * e[5].squeeze(2)
        
        return x
    
    x = cross_attn_ffn(x, context, context_lens, e)
    
    return x
```

---

## 11. 注意力计算

### 11.1 WanSelfAttention.forward()

**文件**: `wan/modules/model.py`, 行 126-155

```python
class WanSelfAttention(nn.Module):
    def forward(self, x, seq_lens, grid_sizes, freqs):
        """
        自注意力 + RoPE
        """
        b, s, n, d = *x.shape[:2], self.num_heads, self.head_dim  # 40, 128
        
        # ========== 步骤1: QKV投影 ==========
        def qkv_fn(x_):
            q = self.norm_q(self.q(x_)).view(b, s, n, d)
            k = self.norm_k(self.k(x_)).view(b, s, n, d)
            v = self.v(x_).view(b, s, n, d)
            return q, k, v
        
        q, k, v = qkv_fn(x)
        
        # ========== 步骤2: 应用RoPE (旋转位置编码) ==========
        q = rope_apply(q, grid_sizes, freqs)
        k = rope_apply(k, grid_sizes, freqs)
        
        # ========== 步骤3: Flash Attention ==========
        x = flash_attention(
            q=q, k=k, v=v,
            k_lens=seq_lens,
            window_size=self.window_size  # (-1, -1) 全局注意力
        )
        
        # ========== 步骤4: 输出投影 ==========
        x = x.flatten(2)  # [B, L, C]
        x = self.o(x)
        
        return x
```

### 11.2 flash_attention()

**文件**: `wan/modules/attention.py`, 行 24-130

```python
def flash_attention(q, k, v, q_lens=None, k_lens=None, ...):
    """
    高效注意力计算
    
    q: [B, Lq, Nq, C]
    k: [B, Lk, Nk, C]  
    v: [B, Lk, Nk, C]
    """
    
    # ========== 步骤1: 预处理 ==========
    # 填充到统一长度
    if q_lens is None:
        q = half(q.flatten(0, 1))
        q_lens = torch.tensor([lq] * b, dtype=torch.int32).to(device)
    else:
        q = half(torch.cat([u[:v] for u, v in zip(q, q_lens)]))
    
    # 同样的方式处理k和v
    k = half(torch.cat([u[:v] for u, v in zip(k, k_lens)]))
    v = half(torch.cat([u[:v] for u, v in zip(v, k_lens)]))
    
    # ========== 步骤2: Flash Attention 调用 ==========
    if FLASH_ATTN_3_AVAILABLE:
        # 使用Flash Attention 3
        x = flash_attn_interface.flash_attn_varlen_func(
            q=q, k=k, v=v,
            cu_seqlens_q=...,
            cu_seqlens_k=...,
            max_seqlen_q=lq,
            max_seqlen_k=lk,
            softmax_scale=...,
            causal=False,
            deterministic=False,
        )[0].unflatten(0, (b, lq))
    else:
        # 使用Flash Attention 2
        x = flash_attn.flash_attn_varlen_func(...)
    
    return x.type(out_dtype)
```

### 11.3 rope_apply() - 旋转位置编码

**文件**: `wan/modules/model.py`, 行 38-66

```python
@torch.amp.autocast('cuda', enabled=False)
def rope_apply(x, grid_sizes, freqs):
    """
    应用旋转位置编码 (RoPE)
    
    x: [B, L, N, C]
    grid_sizes: [B, 3] - (F, H, W) 每个样本的时空尺寸
    freqs: 预计算的旋转频率
    """
    
    n, c = x.size(2), x.size(3) // 2  # N, C/2
    
    # 分离频率
    freqs = freqs.split([c - 2*(c//3), c//3, c//3], dim=1)
    # freqs: (时间频率, 高度频率, 宽度频率)
    
    # 对每个样本应用RoPE
    output = []
    for i, (f, h, w) in enumerate(grid_sizes.tolist()):
        seq_len = f * h * w
        
        # 转换为复数形式
        x_i = torch.view_as_complex(x[i, :seq_len].reshape(seq_len, n, -1, 2))
        
        # 构造位置频率
        freqs_i = torch.cat([
            freqs[0][:f].view(f,1,1,-1).expand(f,h,w,-1),
            freqs[1][:h].view(1,h,1,-1).expand(f,h,w,-1),
            freqs[2][:w].view(1,1,w,-1).expand(f,h,w,-1)
        ], dim=-1).reshape(seq_len, 1, -1)
        
        # 旋转: 复数乘法
        x_i = torch.view_as_real(x_i * freqs_i)
        
        output.append(x_i)
    
    return torch.stack(output).float()
```

---

## 12. VAE解码

### 12.1 WanI2V.generate() - VAE解码

**文件**: `wan/image2video.py`, 行 420-421

```python
# ========== 最终解码 ==========
if self.rank == 0:
    videos = self.vae.decode(x0)
    # x0: [16, F', H', W'] - 纯latent (无噪声)
    # videos: [3, F, H, W] - 像素视频
```

### 12.2 Wan2_1_VAE.decode()

**文件**: `wan/modules/vae2_1.py`

```python
def decode(self, z, scale):
    """
    Latent -> 视频
    """
    # 逆归一化
    if isinstance(scale[0], torch.Tensor):
        z = z / scale[1].view(1, self.z_dim, 1, 1, 1) + scale[0].view(1, self.z_dim, 1, 1, 1)
    else:
        z = z / scale[1] + scale[0]
    
    # 分块解码
    iter_ = z.shape[2]
    x = self.conv2(z)
    
    for i in range(iter_):
        if i == 0:
            out = self.decoder(x[:, :, i:i+1, :, :], ..., first_chunk=True)
        else:
            out_ = self.decoder(x[:, :, i:i+1, :, :], ...)
            out = torch.cat([out, out_], 2)
    
    # Unpatchify
    out = unpatchify(out, patch_size=2)
    
    return out
```

---

## 13. 视频保存

### 13.1 save_video()

**文件**: `generate.py`, 行 550-557

```python
# 生成保存文件名
if args.save_file is None:
    formatted_time = datetime.now().strftime("%Y%m%d_%H%M%S")
    formatted_prompt = args.prompt.replace(" ", "_").replace("/", "_")[:50]
    args.save_file = f"{args.task}_{args.size}_{formatted_prompt}_{formatted_time}.mp4"

# 保存视频
save_video(
    tensor=video[None],        # [1, C, F, H, W]
    save_file=args.save_file,
    fps=cfg.sample_fps,        # 16 fps
    nrow=1,
    normalize=True,
    value_range=(-1, 1)         # video范围是[-1, 1]
)
```

### 13.2 save_video() 实现

**文件**: `wan/utils/utils.py`, 行 90+

```python
def save_video(tensor, save_file, fps=16, nrow=1, normalize=True, value_range=(-1,1)):
    """
    保存视频为MP4文件
    """
    # tensor: [B, C, F, H, W]
    
    # 转换到 [0, 255]
    if normalize:
        tensor = (tensor - value_range[0]) / (value_range[1] - value_range[0])
    tensor = (tensor * 255).clamp(0, 255).byte()
    
    # 转为uint8
    B, C, F, H, W = tensor.shape
    
    # 使用imageio或cv2保存
    # ...
```

---

## 完整调用流程图

```
┌─────────────────────────────────────────────────────────────────────────┐
│                        generate.py:main()                               │
└─────────────────────────────────────────────────────────────────────────┘
                                   │
                                   ▼
┌─────────────────────────────────────────────────────────────────────────┐
│ 1. _parse_args() - 解析命令行参数                                        │
│    - task="i2v-A14B"                                                   │
│    - image="input.jpg"                                                 │
│    - prompt="a cat on beach"                                           │
└─────────────────────────────────────────────────────────────────────────┘
                                   │
                                   ▼
┌─────────────────────────────────────────────────────────────────────────┐
│ 2. Image.open() - 加载输入图像                                          │
│    PIL.Image -> RGB                                                    │
└─────────────────────────────────────────────────────────────────────────┘
                                   │
                                   ▼
┌─────────────────────────────────────────────────────────────────────────┐
│ 3. wan.WanI2V() - 模型初始化                                            │
│    ├─ T5EncoderModel()                                                 │
│    │   └─ umt5_xxl encoder + HuggingfaceTokenizer                      │
│    ├─ Wan2_1_VAE()                                                     │
│    │   └─ Encoder3d + Decoder3d                                        │
│    ├─ WanModel (low_noise_model)                                       │
│    │   └─ 40层 WanAttentionBlock                                       │
│    └─ WanModel (high_noise_model)                                      │
│        └─ 40层 WanAttentionBlock                                       │
└─────────────────────────────────────────────────────────────────────────┘
                                   │
                                   ▼
┌─────────────────────────────────────────────────────────────────────────┐
│ 4. wan_i2v.generate() - 主生成函数                                      │
│                                                                         │
│ 4.1 图像预处理                                                          │
│     TF.to_tensor(img) -> [-1,1]                                        │
│     计算目标尺寸 (根据max_area)                                          │
└─────────────────────────────────────────────────────────────────────────┘
                                   │
                                   ▼
┌─────────────────────────────────────────────────────────────────────────┐
│ 4.2 文本编码: T5EncoderModel.__call__()                                │
│     ├─ HuggingfaceTokenizer()                                          │
│     │   └─ tokenizer(text) -> [1, seq_len]                             │
│     └─ T5Encoder()                                                      │
│         └─ model(ids, mask) -> [1, actual_len, 4096]                  │
└─────────────────────────────────────────────────────────────────────────┘
                                   │
                                   ▼
┌─────────────────────────────────────────────────────────────────────────┐
│ 4.3 VAE编码: Wan2_1_VAE.encode()                                       │
│     ├─ patchify()                                                       │
│     ├─ Encoder3d()                                                     │
│     │   ├─ CausalConv3d                                                │
│     │   ├─ Down_ResidualBlock × n                                      │
│     │   ├─ AttentionBlock                                              │
│     │   └─ CausalConv3d (to z_dim)                                     │
│     └─ 输出: [16, F', H', W']                                          │
│                                                                         │
│ 4.4 构建Mask                                                            │
│     msk: 第一帧=1, 后续帧=0                                              │
│     y = concat([msk, latent]) -> [17, F', H', W']                       │
└─────────────────────────────────────────────────────────────────────────┘
                                   │
                                   ▼
┌─────────────────────────────────────────────────────────────────────────┐
│ 4.5 初始化噪声                                                          │
│     torch.randn([16, F', H', W'])                                       │
└─────────────────────────────────────────────────────────────────────────┘
                                   │
                                   ▼
┌─────────────────────────────────────────────────────────────────────────┐
│ 4.6 采样器初始化: FlowUniPCMultistepScheduler                          │
│     ├─ set_timesteps(40, shift=5.0)                                     │
│     └─ timesteps: [1000, 975, 950, ..., 25]                            │
└─────────────────────────────────────────────────────────────────────────┘
                                   │
                                   ▼
┌─────────────────────────────────────────────────────────────────────────┐
│ 4.7 去噪循环 (40步)                                                     │
│                                                                         │
│  FOR each timestep t:                                                   │
│                                                                         │
│  ┌──────────────────────────────────────────────────────────────────┐ │
│  │ 4.7.1 _prepare_model_for_timestep(t, boundary=900)              │ │
│  │      ├─ t >= 900: return high_noise_model                        │ │
│  │      └─ t < 900: return low_noise_model                          │ │
│  └──────────────────────────────────────────────────────────────────┘ │
│                                   │                                      │
│  ┌──────────────────────────────────────────────────────────────────┐ │
│  │ 4.7.2 model(x, t, context) - WanModel.forward()                 │ │
│  │      ├─ patch_embedding (Conv3d)                                  │ │
│  │      ├─ time_embedding + time_projection                         │ │
│  │      ├─ text_embedding                                            │ │
│  │      ├─ FOR each block (40层):                                    │ │
│  │      │   ├─ WanAttentionBlock.forward()                          │ │
│  │      │   │   ├─ WanSelfAttention + RoPE                          │ │
│  │      │   │   │   └─ flash_attention()                            │ │
│  │      │   │   ├─ WanCrossAttention                                │ │
│  │      │   │   └─ FFN (Gated MLP)                                  │ │
│  │      │   └─ AdaLN 调制                                           │ │
│  │      └─ head() + unpatchify()                                    │ │
│  └──────────────────────────────────────────────────────────────────┘ │
│                                   │                                      │
│  ┌──────────────────────────────────────────────────────────────────┐ │
│  │ 4.7.3 Classifier-Free Guidance                                   │ │
│  │      noise_pred = noise_uncond + scale * (cond - uncond)       │ │
│  └──────────────────────────────────────────────────────────────────┘ │
│                                   │                                      │
│  ┌──────────────────────────────────────────────────────────────────┐ │
│  │ 4.7.4 scheduler.step(noise_pred, t, latent)                     │ │
│  │      └─ FlowUniPCMultistepScheduler.step()                       │ │
│  │          └─ x_{t-1} = x_t - velocity * dt                        │ │
│  └──────────────────────────────────────────────────────────────────┘ │
│                                   │                                      │
│  └─────────────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────────────┘
                                   │
                                   ▼
┌─────────────────────────────────────────────────────────────────────────┐
│ 4.8 VAE解码: Wan2_1_VAE.decode()                                      │
│     ├─ Decoder3d()                                                    │
│     │   ├─ Up_ResidualBlock × n                                       │
│     │   ├─ AttentionBlock                                             │
│     │   └─ CausalConv3d                                               │
│     ├─ unpatchify()                                                   │
│     └─ 输出: [3, F, H, W]                                             │
└─────────────────────────────────────────────────────────────────────────┘
                                   │
                                   ▼
┌─────────────────────────────────────────────────────────────────────────┐
│ 5. save_video() - 保存为MP4                                            │
│    tensor -> [0,255] -> video writer                                   │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## 关键参数汇总

| 参数 | 值 | 说明 |
|------|-----|------|
| `vae_stride` | (4, 8, 8) | 时序4x压缩, 空间8x8压缩 |
| `patch_size` | (1, 2, 2) | 额外2x2空间patchify |
| `dim` | 5120 | Transformer隐藏维度 |
| `num_layers` | 40 | Transformer层数 |
| `num_heads` | 40 | 注意力头数 |
| `boundary` | 900 | 专家切换边界 (0.9×1000) |
| `sample_steps` | 40 | 采样步数 |
| `shift` | 5.0 | 时间偏移参数 |
| `guide_scale` | (3.5, 3.5) | CFG引导强度 |

---

*文档更新时间: 2026年3月*
