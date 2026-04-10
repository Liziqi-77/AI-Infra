# vLLM-Omni 安装与 WAN2.2 模型使用指南

> 本指南详细说明如何安装 vLLM-Omni 并运行 WAN2.2 视频生成模型。

---

## 一、环境要求

### 1.1 基本要求

| 项目 | 要求 |
|------|------|
| 操作系统 | Linux（Windows 目前不支持） |
| Python | 3.12 |
| GPU | NVIDIA CUDA: 计算能力 7.0+ (V100, T4, RTX20xx, A100, L4, H100 等) |

### 1.2 硬件要求（WAN2.2 模型）

| 模型 | 分辨率 | 显存需求 (BF16) | 推荐配置 |
|------|--------|-----------------|----------|
| Wan2.2-T2V-A14B | 720x1280 | ~60 GiB | 1×H100 80GB 或 2×A100 80GB |
| Wan2.2-T2V-A14B | 480x832 | ~40 GiB | 1×A100 80GB |
| Wan2.2-I2V-A14B | 480x832 | ~45 GiB | 1×A100 80GB |
| Wan2.2-TI2V-5B | 480x832 | ~20 GiB | 1×A100 40GB |

---

## 二、安装方式

### 2.1 方式一：从源码安装（推荐）

```bash
# 1. 创建 Python 虚拟环境
uv venv --python 3.12 --seed
source .venv/bin/activate

# 2. 安装 vLLM (CUDA 平台)
uv pip install vllm==0.19.0 --torch-backend=auto

# 3. 克隆并安装 vLLM-Omni
git clone https://github.com/vllm-project/vllm-omni.git
cd vllm-omni
uv pip install -e .

# 4. (可选) 安装 Gradio 演示依赖
uv pip install -e '.[demo]'
```

### 2.2 方式二：使用预构建 Wheel

```bash
# 1. 创建虚拟环境
uv venv --python 3.12 --seed
source .venv/bin/activate

# 2. 安装 vLLM
uv pip install vllm --torch-backend=auto

# 3. 安装 vLLM-Omni
uv pip install vllm-omni
```

### 2.3 方式三：使用 Docker（生产环境推荐）

```bash
# 拉取官方镜像
docker pull vllm/vllm-omni:v0.18.0

# 运行容器（2 GPU 示例）
docker run --runtime nvidia --gpus 2 \
    -v ~/.cache/huggingface:/root/.cache/huggingface \
    --env "HF_TOKEN=$HF_TOKEN" \
    -p 8091:8091 \
    --ipc=host \
    vllm/vllm-omni:v0.18.0 \
    --model Wan-AI/Wan2.2-T2V-A14B-Diffusers --port 8091
```

---

## 三、NPU (Ascend) 平台安装

### 3.1 使用 Docker（推荐）

```bash
# Atlas A2
export IMAGE=quay.io/ascend/vllm-omni:v0.18.0

# Atlas A3
# export IMAGE=quay.io/ascend/vllm-omni:v0.18.0-a3

docker run --rm \
    --name vllm-omni-npu \
    --shm-size=1g \
    --device /dev/davinci0 \
    --device /dev/davinci1 \
    --device /dev/davinci2 \
    --device /dev/davinci3 \
    --device /dev/davinci_manager \
    --device /dev/devmm_svm \
    --device /dev/hisi_hdc \
    -v /usr/local/dcmi:/usr/local/dcmi \
    -v /usr/local/bin/npu-smi:/usr/local/bin/npu-smi \
    -v /usr/local/Ascend/driver/lib64/:/usr/local/Ascend/driver/lib64/ \
    -v /usr/local/Ascend/driver/version.info:/usr/local/Ascend/driver/version.info \
    -v /etc/ascend_install.info:/etc/ascend_install.info \
    -v /root/.cache:/root/.cache \
    -p 8000:8000 \
    -it $IMAGE bash

# 容器内安装 vLLM-Omni
cd /vllm-workspace
git clone -b v0.18.0 https://github.com/vllm-project/vllm-omni.git
cd vllm-omni
pip install -v -e . --no-build-isolation

export VLLM_WORKER_MULTIPROC_METHOD=spawn
```

### 3.2 从源码构建

```bash
# 安装 vLLM
git clone -b v0.18.0 https://github.com/vllm-project/vllm.git
VLLM_TARGET_DEVICE=empty pip install -v -e .

# 安装 vLLM-Ascend
git clone -b v0.18.0rc1 https://github.com/vllm-project/vllm-ascend.git
pip install -v -e .

# 安装 vLLM-Omni
git clone -b v0.18.0 https://github.com/vllm-project/vllm-omni.git
cd vllm-omni
pip install -v -e . --no-build-isolation

export VLLM_WORKER_MULTIPROC_METHOD=spawn
```

---

## 四、WAN2.2 模型使用

### 4.1 支持的 WAN2.2 模型

| 模型名称 | 类型 | 说明 |
|----------|------|------|
| `Wan-AI/Wan2.2-T2V-A14B-Diffusers` | Text-to-Video | 14B 参数，MoE 架构 |
| `Wan-AI/Wan2.2-I2V-A14B-Diffusers` | Image-to-Video | 14B 参数，MoE 架构 |
| `Wan-AI/Wan2.2-TI2V-5B-Diffusers` | Text/Image-to-Video | 5B 参数，密集架构 |

### 4.2 离线推理：文本生成视频 (T2V)

#### 基本用法

```python
from vllm_omni.entrypoints.omni import Omni
from vllm_omni.inputs.data import OmniDiffusionSamplingParams
import torch

# 初始化模型
omni = Omni(model="Wan-AI/Wan2.2-T2V-A14B-Diffusers")

# 设置随机种子
generator = torch.Generator(device="cuda").manual_seed(42)

# 生成视频
frames = omni.generate(
    {"prompt": "A serene lakeside sunrise with mist over the water."},
    OmniDiffusionSamplingParams(
        height=720,
        width=1280,
        num_frames=81,
        num_inference_steps=40,
        guidance_scale=4.0,
        generator=generator,
    ),
)

# 保存视频
from diffusers.utils import export_to_video
export_to_video(frames[0].request_output.images, "output.mp4", fps=24)
```

#### 使用命令行脚本

```bash
cd examples/offline_inference/text_to_video

# 基本用法
python text_to_video.py \
  --prompt "Two anthropomorphic cats in comfy boxing gear fighting on a spotlighted stage." \
  --height 480 \
  --width 832 \
  --num-frames 33 \
  --guidance-scale 4.0 \
  --flow-shift 12.0 \
  --num-inference-steps 40 \
  --fps 16 \
  --output t2v_out.mp4

# 720p 高清生成
python text_to_video.py \
  --prompt "A cinematic aerial shot of a coastal city at sunset." \
  --height 720 \
  --width 1280 \
  --num-frames 81 \
  --guidance-scale 4.0 \
  --flow-shift 5.0 \
  --num-inference-steps 40 \
  --fps 24 \
  --output t2v_720p.mp4
```

### 4.3 离线推理：图像生成视频 (I2V)

```python
from vllm_omni.entrypoints.omni import Omni
from vllm_omni.inputs.data import OmniDiffusionSamplingParams
import torch
from PIL import Image

# 初始化模型
omni = Omni(model="Wan-AI/Wan2.2-I2V-A14B-Diffusers")

# 加载输入图像
image = Image.open("input.jpg").convert("RGB")
image = image.resize((832, 480), Image.Resampling.LANCZOS)

# 设置随机种子
generator = torch.Generator(device="cuda").manual_seed(42)

# 生成视频
frames = omni.generate(
    {
        "prompt": "A cat playing with yarn",
        "multi_modal_data": {"image": image},
    },
    OmniDiffusionSamplingParams(
        height=480,
        width=832,
        num_frames=81,
        num_inference_steps=50,
        guidance_scale=5.0,
        generator=generator,
    ),
)

# 保存视频
from diffusers.utils import export_to_video
export_to_video(frames[0].request_output.images, "i2v_output.mp4", fps=16)
```

#### 使用命令行脚本

```bash
cd examples/offline_inference/image_to_video

# I2V-A14B (MoE)
python image_to_video.py \
  --model Wan-AI/Wan2.2-I2V-A14B-Diffusers \
  --image input.jpg \
  --prompt "A cat playing with yarn" \
  --height 480 \
  --width 832 \
  --num-frames 81 \
  --guidance-scale 5.0 \
  --flow-shift 5.0 \
  --output i2v_output.mp4

# TI2V-5B (统一模型)
python image_to_video.py \
  --model Wan-AI/Wan2.2-TI2V-5B-Diffusers \
  --image input.jpg \
  --prompt "A cinematic dolly shot of a boat" \
  --output ti2v_output.mp4
```

### 4.4 在线服务 (OpenAI API)

#### 启动服务

```bash
# 启动 vLLM-Omni 服务
vllm serve Wan-AI/Wan2.2-T2V-A14B-Diffusers --omni --port 8091
```

#### 调用 API

```bash
# 使用 curl 调用
curl -s http://localhost:8091/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{
    "messages": [
      {"role": "user", "content": "A serene lakeside sunrise with mist over the water."}
    ],
    "extra_body": {
      "height": 720,
      "width": 1280,
      "num_inference_steps": 40,
      "guidance_scale": 4.0,
      "seed": 42
    }
  }' | jq -r '.choices[0].message.content[0].image_url.url' | cut -d',' -f2 | base64 -d > output.mp4
```

---

## 五、高级配置

### 5.1 量化配置

```bash
# 使用 FP8 量化（减少显存）
python text_to_video.py \
  --model Wan-AI/Wan2.2-T2V-A14B-Diffusers \
  --quantization fp8 \
  --prompt "A dog running across a field." \
  --output quantized_output.mp4
```

### 5.2 分布式推理

```bash
# 张量并行 (TP)
python text_to_video.py \
  --model Wan-AI/Wan2.2-T2V-A14B-Diffusers \
  --tensor-parallel-size 2 \
  --prompt "A cinematic shot." \
  --output tp_output.mp4

# 序列并行 (Ulysses)
python text_to_video.py \
  --model Wan-AI/Wan2.2-T2V-A14B-Diffusers \
  --ulysses-degree 2 \
  --prompt "A cinematic shot." \
  --output sp_output.mp4

# CFG 并行
python text_to_video.py \
  --model Wan-AI/Wan2.2-T2V-A14B-Diffusers \
  --cfg-parallel-size 2 \
  --prompt "A cinematic shot." \
  --output cfg_output.mp4
```

### 5.3 内存优化

```bash
# CPU Offload
python text_to_video.py \
  --model Wan-AI/Wan2.2-T2V-A14B-Diffusers \
  --enable-cpu-offload \
  --prompt "A cinematic shot." \
  --output cpu_offload_output.mp4

# Layerwise Offload
python text_to_video.py \
  --model Wan-AI/Wan2.2-T2V-A14B-Diffusers \
  --enable-layerwise-offload \
  --prompt "A cinematic shot." \
  --output layerwise_output.mp4

# VAE Slicing + Tiling
python text_to_video.py \
  --model Wan-AI/Wan2.2-T2V-A14B-Diffusers \
  --vae-use-slicing \
  --vae-use-tiling \
  --prompt "A cinematic shot." \
  --output vae_opt_output.mp4
```

### 5.4 缓存加速 (cache-dit)

```bash
python text_to_video.py \
  --model Wan-AI/Wan2.2-T2V-A14B-Diffusers \
  --cache-backend cache_dit \
  --prompt "A cinematic shot." \
  --output cached_output.mp4
```

---

## 六、WAN2.2 参数说明

### 6.1 核心参数

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `--height` | 视频高度 | 720 |
| `--width` | 视频宽度 | 1280 |
| `--num-frames` | 帧数 | 81 |
| `--num-inference-steps` | 采样步数 | 40 |
| `--guidance-scale` | CFG 缩放因子 | 4.0 |
| `--guidance-scale-high` | 高噪声阶段 CFG（MoE专用） | - |
| `--flow-shift` | 调度器参数 | 5.0 (720p) / 12.0 (480p) |
| `--boundary-ratio` | MoE 边界分割比例 | 0.875 |
| `--fps` | 输出视频帧率 | 24 |

### 6.2 推荐配置

| 分辨率 | flow_shift | guidance_scale | steps | 显存 |
|--------|------------|----------------|-------|------|
| 480p (480x832) | 12.0 | 4.0 | 40 | ~40 GiB |
| 720p (720x1280) | 5.0 | 4.0 | 40 | ~60 GiB |

---

## 七、常见问题

### 7.1 OOM (显存不足)

**解决方案：**

1. 降低分辨率和帧数
2. 启用量化：`--quantization fp8`
3. 启用 CPU Offload：`--enable-cpu-offload`
4. 启用 VAE 优化：`--vae-use-slicing --vae-use-tiling`

```bash
# 快速测试配置
python text_to_video.py \
  --model Wan-AI/Wan2.2-T2V-A14B-Diffusers \
  --height 320 --width 576 --num-frames 17 \
  --num-inference-steps 30 \
  --prompt "A quick test." \
  --output quick_test.mp4
```

### 7.2 模型下载慢

**解决方案：**

设置 HuggingFace 镜像：

```bash
export HF_ENDPOINT=https://hf-mirror.com
```

或使用本地模型路径：

```bash
# 先下载模型
huggingface-cli download Wan-AI/Wan2.2-T2V-A14B-Diffusers --local-dir ./models/wan22-t2v

# 使用本地路径
python text_to_video.py --model ./models/wan22-t2v ...
```

### 7.3 NPU 环境问题

**检查 NPU 状态：**

```bash
npu-smi info
```

**确保环境变量正确：**

```bash
export VLLM_WORKER_MULTIPROC_METHOD=spawn
```

---

## 八、参考链接

- [vLLM-Omni 官方文档](https://vllm-omni.readthedocs.io/en/latest/)
- [vLLM-Omni GitHub](https://github.com/vllm-project/vllm-omni)
- [支持的模型列表](https://vllm-omni.readthedocs.io/en/latest/models/supported_models/)
- [vLLM-Omni 论文](https://arxiv.org/abs/2602.02204)
- [vLLM-Ascend (NPU)](https://docs.vllm.ai/projects/ascend/)
