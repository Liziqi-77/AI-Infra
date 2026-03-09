# PyTorch量化核心代码逐行解释

本文档详细解释 `src/fouroversix/quantize/pytorch/reference.py` 中的量化核心代码，包括NVFP4、MXFP4以及它们的FourOverSix自适应量化算法。

---

## 目录

1. [常量定义](#1-常量定义)
2. [E2M1伪量化函数](#2-e2m1伪量化函数-fake_quantize_to_e2m1)
3. [BF16到FP4转换函数](#3-bf16到fp4转换函数-quantize_bf16_to_unpacked_fp4)
4. [FP4打包函数](#4-fp4打包函数-pack_unpacked_fp4)
5. [MXFP4量化函数](#5-mxfp4量化函数-quantize_to_mxfp4)
6. [MXFP4 FourOverSix选择函数](#6-mxfp4-fouroversix选择函数-select_fouroversix_mxfp4)
7. [NVFP4量化函数](#7-nvfp4量化函数-quantize_to_nvfp4)
8. [NVFP4 FourOverSix选择函数](#8-nvfp4-fouroversix选择函数-select_fouroversix)
9. [主量化函数](#9-主量化函数-quantize_to_fp4)

---

## 1. 常量定义

```python
E2M1_MAX_VALUE = 6          # E2M1格式的最大可表示值
E2M1_MAX_FOUR = 4           # E2M1格式的次大值（用于4/6方案B）
E4M3_MAX_VALUE = 448        # E4M3格式的最大可表示值（标准NVFP4）
E4M3_MAX_FOUROVERSIX = 256  # E4M3格式的最大值（FourOverSix自适应量化）
```

**解释**：
- **E2M1格式**：1位符号 + 2位指数 + 1位尾数，可表示值：0, 0.5, 1, 1.5, 2, 3, 4, 6
- **E4M3格式**：1位符号 + 4位指数 + 3位尾数，用于NVFP4的缩放因子
- **E8M0格式**：8位指数 + 0位尾数，用于MXFP4的缩放因子

---

## 2. E2M1伪量化函数 (fake_quantize_to_e2m1)

```python
def fake_quantize_to_e2m1(
    x: torch.Tensor,
    *,
    round_style: RoundStyle = RoundStyle.nearest,
) -> torch.Tensor:
```

**功能**：将浮点数量化为E2M1格式的伪量化函数，返回的是浮点数形式的E2M1值（而非编码）。

### 逐行解释

```python
# 第18-21行：最近邻舍入方式
if round_style == RoundStyle.nearest:
    step1 = torch.round(2 * x.abs()) / 2  # 对于 |x| < 2，步长为0.5
    step2 = torch.round(x.abs())          # 对于 2 <= |x| < 4，步长为1
    step3 = 2 * torch.round(x.abs() / 2)  # 对于 |x| >= 4，步长为2
```

**解释**：
- E2M1格式在不同数值范围有不同的量化步长：
  - `|x| < 2`：可表示 0, 0.5, 1, 1.5，步长为0.5
  - `2 <= |x| < 4`：可表示 2, 3，步长为1
  - `|x| >= 4`：可表示 4, 6，步长为2

```python
# 第22-27行：随机舍入方式（用于训练）
elif round_style == RoundStyle.stochastic:
    rbits = torch.rand_like(x.abs()) - 0.5  # 生成[-0.5, 0.5)的随机数
    step1 = torch.round(2 * x.abs() + rbits) / 2
    step2 = torch.round(x.abs() + rbits)
    step3 = 2 * torch.round(x.abs() / 2 + rbits)
    step3[step3 > E2M1_MAX_VALUE] = E2M1_MAX_VALUE  # 限制最大值为6
```

**解释**：
- 随机舍入通过添加随机噪声来实现，有助于减少量化偏差
- 需要限制最大值不超过6（E2M1的最大可表示值）

```python
# 第29-34行：根据数值范围选择合适的量化步长
mask1 = x.abs() < 2   # |x| < 2 的掩码
mask2 = x.abs() < 4   # |x| < 4 的掩码

return x.sign() * (
    step1 * mask1 +                    # |x| < 2 使用step1
    step2 * (~mask1) * mask2 +         # 2 <= |x| < 4 使用step2
    step3 * (~mask1) * (~mask2)        # |x| >= 4 使用step3
)
```

**解释**：
- 使用布尔掩码来选择正确的量化步长
- `~mask1 * mask2` 表示 `2 <= |x| < 4`
- `~mask1 * ~mask2` 表示 `|x| >= 4`
- 最后乘以 `x.sign()` 恢复符号

---

## 3. BF16到FP4转换函数 (quantize_bf16_to_unpacked_fp4)

```python
def quantize_bf16_to_unpacked_fp4(x: torch.Tensor) -> torch.Tensor:
    assert x.dtype == torch.bfloat16
```

**功能**：通过位操作将BF16浮点数直接转换为E2M1编码（未打包形式），每个值占一个uint8。

### BF16格式回顾

```
BF16: [S|EEEEEEEE|MMMMMMM]  (1位符号 + 8位指数 + 7位尾数)
E2M1: [S|EE|M]              (1位符号 + 2位指数 + 1位尾数)
```

### 逐行解释

```python
# 第40-44行：解析BF16的各个字段
bx = x.view(torch.int16)              # 将BF16视为int16进行位操作
s = (bx >> 15) & 0x1                  # 提取符号位（第15位）
e = (bx >> 7) & 0xFF                  # 提取指数位（第7-14位）
m = bx & 0x7F                         # 提取尾数位（第0-6位）
is_zero = (e == 0) & (m == 0)         # 判断是否为零（指数和尾数都为0）
```

**解释**：
- BF16的指数偏置为127，即实际指数 = e - 127
- 零值的指数和尾数都为0

```python
# 第46-49行：处理尾数
m = (m >> 6) & 1                      # 只保留最高位尾数（E2M1只有1位尾数）
is_half = (e == 126) & (m == 0)       # 检测0.5的特殊情况
m = torch.where(is_half, torch.tensor(1, dtype=torch.int16, device=x.device), m)
```

**解释**：
- E2M1只有1位尾数，所以只保留BF16尾数的最高位
- **特殊情况**：0.5在BF16中指数为126（偏置127-1=126），尾数为0
  - 但在E2M1中，0.5应该编码为 `E=0, M=1`（次正规数）
  - 所以需要特殊处理，将尾数设为1

```python
# 第51-57行：指数映射
# exp=126 -> E=0 (次正规数: 0, 0.5)
# exp=127 -> E=1 (正规数: 1, 1.5)
# exp=128 -> E=2 (正规数: 2, 3)
# exp=129 -> E=3 (正规数: 4, 6)
e = e - 126                           # 将BF16指数转换为E2M1指数
e = torch.where(is_zero, torch.tensor(0, dtype=torch.int16, device=x.device), e)
```

**解释**：
- E2M1的指数范围是0-3，对应BF16的指数126-129
- 零值的指数设为0

```python
# 第59-63行：组合成E2M1编码
m = torch.where(is_zero, torch.tensor(0, dtype=torch.int16, device=x.device), m)
code = (s << 3) | (e << 1) | m        # 组合：[S|EE|M] -> 4位编码
return code.to(torch.uint8)           # 返回uint8类型
```

**解释**：
- E2M1编码格式：`[S|E1|E0|M]`
- 例如：`6.0` -> `S=0, E=3, M=0` -> `0110` = 6
- 例如：`-4.0` -> `S=1, E=3, M=0` -> `1110` = 14

---

## 4. FP4打包函数 (pack_unpacked_fp4)

```python
def pack_unpacked_fp4(x: torch.Tensor) -> torch.Tensor:
    assert x.dtype == torch.uint8
```

**功能**：将未打包的FP4值（每个值占一个uint8的低4位）打包为紧凑格式（每个uint8存储2个FP4值）。

### 逐行解释

```python
# 第69-71行：计算打包后的维度
dim = 1                                    # 沿第1维（列方向）打包
size_along_dim = x.size(dim)               # 原始列数
new_size_along_dim = (size_along_dim + 1) // 2  # 打包后列数（向上取整）
```

```python
# 第73-78行：处理奇数长度
if size_along_dim % 2 != 0:
    pad_sizes = [0] * (2 * x.ndim)         # 初始化填充列表
    pad_index = (x.ndim - dim - 1) * 2 + 1 # 计算填充位置索引
    pad_sizes[pad_index] = 1               # 在末尾填充1个元素
    x = torch.nn.functional.pad(x, pad_sizes, mode="constant", value=0)
```

**解释**：
- 如果列数是奇数，需要在末尾填充一个0，以便成对打包

```python
# 第80-87行：重塑并打包
new_shape = list(x.shape)
new_shape[dim] = new_size_along_dim
new_shape.insert(dim + 1, 2)              # 插入新维度，大小为2
x = x.reshape(*new_shape)                 # 重塑为 [..., new_size, 2]

low = x.select(dim + 1, 0)                # 选择低4位值
high = x.select(dim + 1, 1)               # 选择高4位值
return (high << 4) | low                  # 组合：高4位 << 4 | 低4位
```

**解释**：
- 打包后的uint8：`[HIGH4|LOW4]`
- 例如：`low=0110(6), high=1110(14)` -> `11100110` = 230

---

## 5. MXFP4量化函数 (quantize_to_mxfp4)

```python
def quantize_to_mxfp4(
    x_scale_blocks: torch.Tensor,          # [num_blocks, 32] 数据块
    *,
    scale_rule: ScaleRule = ScaleRule.mse,
) -> tuple[torch.Tensor, torch.Tensor]:
```

**功能**：执行MXFP4格式的量化，返回缩放后的数据块和E8M0格式的缩放因子。

### MXFP4与NVFP4的关键区别

| 特性 | MXFP4 | NVFP4 |
|------|-------|-------|
| 块大小 | 32 | 16 |
| 缩放因子格式 | E8M0 (仅指数) | E4M3 (指数+尾数) |
| 全局amax | 不需要 | 需要 |

### 逐行解释

```python
# 第102-104行：计算原始缩放因子
x_scales_hp = (
    x_scale_blocks.abs().max(axis=-1).values  # 每块的最大绝对值
    / scale_rule.max_allowed_e2m1_value()     # 除以6或4
)
```

**解释**：
- `x_scale_blocks.abs().max(axis=-1).values`：计算每个32元素块的最大绝对值
- `scale_rule.max_allowed_e2m1_value()`：返回6（static_6）或4（static_4）
- 结果：`scale = max(|x_block|) / 6` 或 `scale = max(|x_block|) / 4`

```python
# 第106-109行：提取E8M0格式的指数
x_scales_e8m0_u32 = x_scales_hp.view(torch.int32)  # 将float32视为int32
x_scales_e8m0 = ((x_scales_e8m0_u32 >> 23) & 0xFF).to(torch.uint8)  # 提取8位指数
```

**解释**：
- IEEE 754 float32格式：`[S|EEEEEEEE|MMMMMMMMMMMMMMMMMMMMMMM]` (1+8+23)
- E8M0格式只保留指数部分，所以提取第23-30位
- `>> 23` 将指数移到最低8位

```python
# 第111-116行：向上取整
x_scales = torch.where(
    (x_scales_e8m0_u32 & 0x7FFFFF) == 0,   # 检查尾数是否为0
    x_scales_e8m0,                          # 尾数为0，不需要调整
    x_scales_e8m0 + 1,                      # 尾数不为0，指数+1（向上取整）
)
```

**解释**：
- E8M0格式没有尾数，只能表示2的幂次
- 为了保证量化范围覆盖原始数据，需要向上取整
- 例如：`scale = 3.5` -> `2^2 = 4`（向上取整到最近的2的幂）

```python
# 第118-123行：转换回浮点数并缩放数据块
x_scales_hp = (x_scales.to(torch.int32) << 23).view(torch.float32)  # 重构float32
x_block_scaled = x_scale_blocks / x_scales_hp.unsqueeze(1)          # 缩放数据块

return x_block_scaled, x_scales.view(torch.float8_e8m0fnu)
```

**解释**：
- `(x_scales.to(torch.int32) << 23).view(torch.float32)`：将E8M0指数重构为float32
  - 例如：`exponent = 2` -> `2 << 23 = 0x40000000` -> `float32 = 4.0`
- `x_scale_blocks / x_scales_hp.unsqueeze(1)`：将数据块除以缩放因子
- 返回的缩放因子类型为 `torch.float8_e8m0fnu`

---

## 6. MXFP4 FourOverSix选择函数 (select_fouroversix_mxfp4)

```python
def select_fouroversix_mxfp4(
    x_scale_blocks: torch.Tensor,          # [num_blocks, 32] 原始数据块
    x_block_scaled_6: torch.Tensor,        # 方案A(max=6)的缩放后数据
    scales_6: torch.Tensor,                # 方案A的E8M0缩放因子
    x_block_scaled_4: torch.Tensor,        # 方案B(max=4)的缩放后数据
    scales_4: torch.Tensor,                # 方案B的E8M0缩放因子
    *,
    scale_rule: ScaleRule = ScaleRule.mse,
    round_style: RoundStyle = RoundStyle.nearest,
) -> tuple[torch.Tensor, torch.Tensor]:
```

**功能**：为MXFP4格式执行FourOverSix自适应选择，比较max=6和max=4两种方案的量化误差，选择更优的方案。

### 逐行解释

```python
# 第154-162行：对两种方案进行伪量化
x_fake_quantized_6 = fake_quantize_to_e2m1(
    x_block_scaled_6,
    round_style=round_style,
)
x_fake_quantized_4 = fake_quantize_to_e2m1(
    x_block_scaled_4,
    round_style=round_style,
)
```

**解释**：
- 将两种方案的缩放后数据分别量化为E2M1格式
- `x_block_scaled_6`：使用 `scale = max/6` 缩放后的数据
- `x_block_scaled_4`：使用 `scale = max/4` 缩放后的数据

```python
# 第164-178行：反量化以计算误差
# MXFP4使用E8M0格式缩放因子，需要将其转换回float32
scales_6_hp = (scales_6.view(torch.uint8).to(torch.int32) << 23).view(torch.float32)
scales_4_hp = (scales_4.view(torch.uint8).to(torch.int32) << 23).view(torch.float32)

# 反量化公式: x_dequantized = x_e2m1 * scale_e8m0
# 注意：MXFP4没有全局amax，直接使用E8M0缩放因子
x_dequantized_6 = (
    x_fake_quantized_6.to(torch.float32)
    * scales_6_hp.unsqueeze(1)
)
x_dequantized_4 = (
    x_fake_quantized_4.to(torch.float32)
    * scales_4_hp.unsqueeze(1)
)
```

**解释**：
- **关键区别**：MXFP4的反量化不需要全局amax
- 反量化公式：`x_dequantized = x_e2m1 * scale_e8m0`
- E8M0到float32的转换：`(exponent << 23).view(torch.float32)`

```python
# 第180-189行：计算量化误差
if scale_rule == ScaleRule.abs_max:
    x_error_4 = (x_dequantized_4 - x_scale_blocks).abs().max(axis=-1).values
    x_error_6 = (x_dequantized_6 - x_scale_blocks).abs().max(axis=-1).values
elif scale_rule == ScaleRule.mae:
    x_error_4 = (x_dequantized_4 - x_scale_blocks).abs().sum(axis=-1)
    x_error_6 = (x_dequantized_6 - x_scale_blocks).abs().sum(axis=-1)
elif scale_rule == ScaleRule.mse:
    x_error_4 = ((x_dequantized_4 - x_scale_blocks) ** 2).sum(axis=-1)
    x_error_6 = ((x_dequantized_6 - x_scale_blocks) ** 2).sum(axis=-1)
```

**解释**：
- **abs_max**：最大绝对误差，`max(|x_original - x_dequantized|)`
- **mae**：平均绝对误差之和，`sum(|x_original - x_dequantized|)`
- **mse**：均方误差之和，`sum((x_original - x_dequantized)^2)`

```python
# 第191-204行：选择误差更小的方案
select_4 = (x_error_4 < x_error_6).unsqueeze(1)  # 选择方案B的条件
x_fake_quantized = torch.where(
    select_4,
    x_fake_quantized_4.reshape(x_scale_blocks.shape[0], -1),
    x_fake_quantized_6.reshape(x_scale_blocks.shape[0], -1),
)
scales = torch.where(
    select_4,
    scales_4.reshape(-1, 1),
    scales_6.reshape(-1, 1),
)

return x_fake_quantized, scales
```

**解释**：
- `select_4`：布尔张量，True表示方案B（max=4）误差更小
- 使用 `torch.where` 根据条件选择结果
- 返回选择的伪量化结果和对应的缩放因子

---

## 7. NVFP4量化函数 (quantize_to_nvfp4)

```python
def quantize_to_nvfp4(
    x_scale_blocks: torch.Tensor,          # [num_blocks, 16] 数据块
    x_amax: torch.Tensor,                  # 全局最大值
    *,
    scale_rule: ScaleRule,
    scale_expansion_factor: float | None = None,  # 缩放因子扩展系数
) -> tuple[torch.Tensor, torch.Tensor]:
```

**功能**：执行NVFP4格式的量化，返回缩放后的数据块和E4M3格式的缩放因子。

### 逐行解释

```python
# 第214-238行：计算缩放因子
if x_amax == 0:
    x_scales_hp = torch.zeros(...)        # 零张量的缩放因子为0
else:
    encode_scale = (
        torch.tensor(
            scale_rule.max_allowed_e2m1_value()    # 6 或 4
            * scale_rule.max_allowed_e4m3_value(), # 448 或 256
            dtype=x_amax.dtype,
            device=x_amax.device,
        )
        / x_amax
    )
    x_scales_hp = (
        x_scale_blocks.abs().max(axis=-1).values   # 每块的最大绝对值
        / torch.tensor(
            scale_rule.max_allowed_e2m1_value(),   # 6 或 4
            dtype=x_amax.dtype,
            device=x_amax.device,
        )
        * encode_scale
    )
```

**解释**：
- **encode_scale**：编码缩放因子，用于将缩放因子归一化到E4M3的表示范围
  - 公式：`encode_scale = (max_e2m1 * max_e4m3) / amax`
  - 对于标准NVFP4：`encode_scale = (6 * 448) / amax`
  - 对于FourOverSix：`encode_scale = (6 * 256) / amax`
- **x_scales_hp**：高精度缩放因子
  - 公式：`scale = (max(|x_block|) / max_e2m1) * encode_scale`

```python
# 第240-243行：可选的缩放因子扩展
if scale_expansion_factor is not None:
    x_scales_hp = x_scales_hp * scale_expansion_factor

x_scales = x_scales_hp.to(torch.float8_e4m3fn)  # 转换为E4M3格式
```

**解释**：
- `scale_expansion_factor`：用于FourOverSix方案B
  - 方案A（max=6）：`scale_expansion_factor = None`
  - 方案B（max=4）：`scale_expansion_factor = 1.5`
- 转换为E4M3格式时会有精度损失

```python
# 第245-257行：计算解码缩放因子并缩放数据块
decode_scale = 1 / (
    torch.tensor(
        scale_rule.max_allowed_e2m1_value() * scale_rule.max_allowed_e4m3_value(),
        dtype=x_amax.dtype,
        device=x_amax.device,
    )
    / x_amax
)
x_block_scaled = torch.where(
    x_scales.unsqueeze(1) != 0,
    x_scale_blocks * (1 / (decode_scale * x_scales.to(x_amax.dtype).unsqueeze(1))),
    0,
)

return x_block_scaled, x_scales
```

**解释**：
- **decode_scale**：解码缩放因子，用于反量化
  - 公式：`decode_scale = amax / (max_e2m1 * max_e4m3)`
- **x_block_scaled**：缩放后的数据块
  - 公式：`x_scaled = x_block / (decode_scale * scale)`
  - 这使得缩放后的数据范围适合E2M1表示

---

## 8. NVFP4 FourOverSix选择函数 (select_fouroversix)

```python
def select_fouroversix(
    x_scale_blocks: torch.Tensor,          # [num_blocks, 16] 原始数据块
    x_block_scaled_6: torch.Tensor,        # 方案A(max=6)的缩放后数据
    scales_6: torch.Tensor,                # 方案A的E4M3缩放因子
    x_block_scaled_4: torch.Tensor,        # 方案B(max=4)的缩放后数据
    scales_4: torch.Tensor,                # 方案B的E4M3缩放因子
    x_amax: torch.Tensor,                  # 全局最大值
    *,
    scale_rule: ScaleRule = ScaleRule.mse,
    round_style: RoundStyle = RoundStyle.nearest,
) -> tuple[torch.Tensor, torch.Tensor]:
```

**功能**：为NVFP4格式执行FourOverSix自适应选择。

### 逐行解释

```python
# 第273-280行：对两种方案进行伪量化
x_fake_quantized_6 = fake_quantize_to_e2m1(
    x_block_scaled_6,
    round_style=round_style,
)
x_fake_quantized_4 = fake_quantize_to_e2m1(
    x_block_scaled_4,
    round_style=round_style,
)
```

```python
# 第282-301行：反量化以计算误差
x_dequantized_6 = (
    x_fake_quantized_6.to(x_amax.dtype)
    * scales_6.unsqueeze(1).to(x_amax.dtype)
    * x_amax
    / torch.tensor(
        E2M1_MAX_VALUE * E4M3_MAX_FOUROVERSIX,  # 6 * 256 = 1536
        dtype=x_amax.dtype,
        device=x_amax.device,
    )
)
x_dequantized_4 = (
    x_fake_quantized_4.to(x_amax.dtype)
    * scales_4.unsqueeze(1).to(x_amax.dtype)
    * x_amax
    / torch.tensor(
        E2M1_MAX_VALUE * E4M3_MAX_FOUROVERSIX,  # 6 * 256 = 1536
        dtype=x_amax.dtype,
        device=x_amax.device,
    )
)
```

**解释**：
- **NVFP4反量化公式**：`x_dequantized = x_e2m1 * scale * amax / (max_e2m1 * max_e4m3)`
- 注意这里使用 `E4M3_MAX_FOUROVERSIX = 256`（而非标准的448）
- 这是因为FourOverSix需要更大的缩放因子范围来适应max=4和max=6的选择

```python
# 第303-311行：计算量化误差
if scale_rule == ScaleRule.abs_max:
    x_error_4 = (x_dequantized_4 - x_scale_blocks).abs().max(axis=-1).values
    x_error_6 = (x_dequantized_6 - x_scale_blocks).abs().max(axis=-1).values
elif scale_rule == ScaleRule.mae:
    x_error_4 = (x_dequantized_4 - x_scale_blocks).abs().sum(axis=-1)
    x_error_6 = (x_dequantized_6 - x_scale_blocks).abs().sum(axis=-1)
elif scale_rule == ScaleRule.mse:
    x_error_4 = ((x_dequantized_4 - x_scale_blocks) ** 2).sum(axis=-1)
    x_error_6 = ((x_dequantized_6 - x_scale_blocks) ** 2).sum(axis=-1)
```

```python
# 第313-325行：选择误差更小的方案
select_4 = (x_error_4 < x_error_6).unsqueeze(1)
x_fake_quantized = torch.where(
    select_4,
    x_fake_quantized_4.reshape(x_scale_blocks.shape[0], -1),
    x_fake_quantized_6.reshape(x_scale_blocks.shape[0], -1),
)
scales = torch.where(
    select_4,
    scales_4.reshape(-1, 1),
    scales_6.reshape(-1, 1),
)

return x_fake_quantized, scales
```

---

## 9. 主量化函数 (quantize_to_fp4)

```python
def quantize_to_fp4(
    x: torch.Tensor,                       # 输入张量 [M, N]
    x_amax: torch.Tensor | None = None,    # 可选的全局最大值
    had: torch.Tensor | None = None,       # 可选的Hadamard矩阵
    *,
    block_scale_2d: bool = False,          # 是否使用2D块缩放
    fp4_format: DataType = DataType.nvfp4, # 数据类型 (nvfp4/mxfp4)
    round_style: RoundStyle = RoundStyle.nearest,
    scale_rule: ScaleRule = ScaleRule.mse,
    transpose: bool = False,               # 是否转置
) -> tuple[torch.Tensor, torch.Tensor, torch.Tensor | None]:
```

**功能**：主量化入口函数，根据配置执行相应的量化流程。

### 逐行解释

```python
# 第342-346行：可选的转置和Hadamard变换
if transpose:
    x = x.T

if had is not None:
    x = (x.reshape(-1, had.shape[0]) @ had).reshape_as(x)
```

**解释**：
- `transpose`：用于权重矩阵的转置存储
- `had`：随机Hadamard变换，用于减少离群值的影响

```python
# 第348-353行：计算全局最大值
if x_amax is None:
    x_amax = (
        torch.ones(1, device=x.device, dtype=x.dtype)
        if fp4_format == DataType.mxfp4
        else x.abs().max().float()
    )
```

**解释**：
- **MXFP4**：不需要全局amax，设为1（因为使用E8M0格式，每块独立缩放）
- **NVFP4**：需要全局amax，用于缩放因子的归一化

```python
# 第355-371行：分块
if block_scale_2d:
    # 2D块缩放 (16x16 或 32x32)
    x_scale_blocks = (
        x.reshape(
            -1,
            fp4_format.block_size(),
            x.shape[1] // fp4_format.block_size(),
            fp4_format.block_size(),
        )
        .permute(0, 2, 1, 3)
        .reshape(-1, fp4_format.block_size() ** 2)
        .float()
    )
else:
    # 1D块缩放 (1x16 或 1x32)
    x_scale_blocks = x.reshape(-1, fp4_format.block_size()).float()
```

**解释**：
- **1D块缩放**：沿列方向分块，每块大小为16（NVFP4）或32（MXFP4）
- **2D块缩放**：同时沿行和列方向分块，形成方形块

```python
# 第373-444行：根据格式和规则选择量化方法
x_fake_quantized = None

# [MODIFIED] 重构量化分支逻辑，支持MXFP4的自适应量化
if fp4_format == DataType.mxfp4:
    # MXFP4量化分支
    if scale_rule.is_adaptive():
        # [NEW] MXFP4自适应量化 (FourOverSix for MXFP4)
        # 方案A: max=6
        x_block_scaled_6, scales_6 = quantize_to_mxfp4(
            x_scale_blocks,
            scale_rule=ScaleRule.static_6,
        )
        # 方案B: max=4
        x_block_scaled_4, scales_4 = quantize_to_mxfp4(
            x_scale_blocks,
            scale_rule=ScaleRule.static_4,
        )
        # 调用MXFP4的自适应选择函数
        x_fake_quantized, scales = select_fouroversix_mxfp4(
            x_scale_blocks,
            x_block_scaled_6,
            scales_6,
            x_block_scaled_4,
            scales_4,
            scale_rule=scale_rule,
            round_style=round_style,
        )
    else:
        # MXFP4静态量化 (static_6 或 static_4)
        x_block_scaled, scales = quantize_to_mxfp4(
            x_scale_blocks,
            scale_rule=scale_rule,
        )
```

**解释**：
- MXFP4自适应量化流程：
  1. 分别计算max=6和max=4的缩放因子和缩放后数据
  2. 调用 `select_fouroversix_mxfp4` 选择更优方案

```python
elif fp4_format == DataType.nvfp4 and scale_rule in {
    ScaleRule.static_6,
    ScaleRule.static_4,
}:
    # NVFP4静态量化
    x_block_scaled, scales = quantize_to_nvfp4(
        x_scale_blocks,
        x_amax,
        scale_rule=scale_rule,
    )
elif fp4_format == DataType.nvfp4:  # Four over six for NVFP4
    # NVFP4自适应量化
    x_block_scaled_6, scales_6 = quantize_to_nvfp4(
        x_scale_blocks,
        x_amax,
        scale_rule=scale_rule,
    )
    x_block_scaled_4, scales_4 = quantize_to_nvfp4(
        x_scale_blocks,
        x_amax,
        scale_rule=scale_rule,
        scale_expansion_factor=1.5,  # 方案B需要扩展缩放因子
    )
    x_fake_quantized, scales = select_fouroversix(
        x_scale_blocks,
        x_block_scaled_6,
        scales_6,
        x_block_scaled_4,
        scales_4,
        x_amax,
        scale_rule=scale_rule,
        round_style=round_style,
    )
```

**解释**：
- NVFP4自适应量化与MXFP4的关键区别：
  - 方案B使用 `scale_expansion_factor=1.5` 扩展缩放因子
  - 需要传入全局 `x_amax`

```python
# 第446-450行：如果还没进行伪量化，则执行
if x_fake_quantized is None:
    x_fake_quantized = fake_quantize_to_e2m1(
        x_block_scaled,
        round_style=round_style,
    )
```

```python
# 第452-472行：处理2D块缩放的形状调整
if block_scale_2d:
    x_fake_quantized = x_fake_quantized.reshape(
        -1,
        x.shape[1] // fp4_format.block_size(),
        fp4_format.block_size(),
        fp4_format.block_size(),
    ).permute(0, 2, 1, 3)

    scales = (
        scales.reshape(
            1,
            x.shape[0] // fp4_format.block_size(),
            x.shape[1] // fp4_format.block_size(),
        )
        .broadcast_to(
            fp4_format.block_size(),
            x.shape[0] // fp4_format.block_size(),
            x.shape[1] // fp4_format.block_size(),
        )
        .permute(1, 0, 2)
    )
```

```python
# 第474-485行：打包和布局转换
x_quantized = pack_unpacked_fp4(
    quantize_bf16_to_unpacked_fp4(x_fake_quantized.bfloat16().reshape_as(x)),
)

reshaped_scales = to_blocked(
    scales.reshape(
        x.shape[0],
        x.shape[1] // fp4_format.block_size(),
    ),
)

return x_quantized, reshaped_scales, x_amax
```

**解释**：
1. 将伪量化结果转换为BF16，然后转换为E2M1编码
2. 打包为紧凑的uint8格式
3. 将缩放因子转换为Blackwell GPU要求的blocked布局
4. 返回：量化后的值、缩放因子、全局amax

---

## 10. 完整量化流程图

### 10.1 NVFP4标准量化流程

```
输入 x [M, N]
    │
    ▼
计算全局amax = max(|x|)
    │
    ▼
分块 reshape为 [M*N/16, 16]
    │
    ▼
quantize_to_nvfp4():
    │
    ├── 计算encode_scale = (6 * 448) / amax
    │
    ├── 计算每块缩放因子
    │   scale_hp = max(|x_block|) / 6 * encode_scale
    │
    ├── 转换为E4M3格式
    │   scale = scale_hp.to(float8_e4m3)
    │
    └── 缩放数据块
        x_scaled = x_block / (decode_scale * scale)
    │
    ▼
fake_quantize_to_e2m1(x_scaled)
    │
    ▼
quantize_bf16_to_unpacked_fp4() → pack_unpacked_fp4()
    │
    ▼
to_blocked(scales)
    │
    ▼
输出: values (uint8), scales (E4M3), amax (float32)
```

### 10.2 NVFP4 FourOverSix量化流程

```
输入 x [M, N]
    │
    ▼
计算全局amax = max(|x|)
    │
    ▼
分块 reshape为 [M*N/16, 16]
    │
    ├─────────────────────────────────────┐
    │                                     │
    ▼                                     ▼
quantize_to_nvfp4(scale_rule=mse)   quantize_to_nvfp4(scale_rule=mse, scale_expansion_factor=1.5)
    │                                     │
    │ 方案A (max=6)                       │ 方案B (max=4)
    │ scale_6 = max/6                     │ scale_4 = max/6 * 1.5
    │                                     │
    └──────────────┬──────────────────────┘
                   │
                   ▼
            select_fouroversix():
                   │
                   ├── 伪量化两种方案
                   │   x_q_6 = fake_quantize(x_scaled_6)
                   │   x_q_4 = fake_quantize(x_scaled_4)
                   │
                   ├── 反量化
                   │   x_dq_6 = x_q_6 * scale_6 * amax / (6 * 256)
                   │   x_dq_4 = x_q_4 * scale_4 * amax / (6 * 256)
                   │
                   ├── 计算误差
                   │   error_6 = metric(x_block, x_dq_6)
                   │   error_4 = metric(x_block, x_dq_4)
                   │
                   └── 选择更优方案
                       select_4 = (error_4 < error_6)
                   │
                   ▼
打包和布局转换
                   │
                   ▼
输出: values, scales, amax
```

### 10.3 MXFP4标准量化流程

```
输入 x [M, N]
    │
    ▼
分块 reshape为 [M*N/32, 32]
    │
    ▼
quantize_to_mxfp4():
    │
    ├── 计算原始缩放因子
    │   scale_hp = max(|x_block|) / 6
    │
    ├── 提取E8M0指数
    │   exponent = (scale_hp.view(int32) >> 23) & 0xFF
    │
    ├── 向上取整
    │   scale = exponent + (mantissa != 0)
    │
    └── 缩放数据块
        x_scaled = x_block / scale_e8m0
    │
    ▼
fake_quantize_to_e2m1(x_scaled)
    │
    ▼
打包和布局转换
    │
    ▼
输出: values (uint8), scales (E8M0), amax=None
```

### 10.4 MXFP4 FourOverSix量化流程

```
输入 x [M, N]
    │
    ▼
分块 reshape为 [M*N/32, 32]
    │
    ├─────────────────────────────────────┐
    │                                     │
    ▼                                     ▼
quantize_to_mxfp4(static_6)         quantize_to_mxfp4(static_4)
    │                                     │
    │ 方案A (max=6)                       │ 方案B (max=4)
    │ scale_6 = max/6 (E8M0)              │ scale_4 = max/4 (E8M0)
    │                                     │
    └──────────────┬──────────────────────┘
                   │
                   ▼
            select_fouroversix_mxfp4():
                   │
                   ├── 伪量化两种方案
                   │   x_q_6 = fake_quantize(x_scaled_6)
                   │   x_q_4 = fake_quantize(x_scaled_4)
                   │
                   ├── 反量化 (注意：无amax)
                   │   x_dq_6 = x_q_6 * scale_6_e8m0
                   │   x_dq_4 = x_q_4 * scale_4_e8m0
                   │
                   ├── 计算误差
                   │   error_6 = metric(x_block, x_dq_6)
                   │   error_4 = metric(x_block, x_dq_4)
                   │
                   └── 选择更优方案
                       select_4 = (error_4 < error_6)
                   │
                   ▼
打包和布局转换
                   │
                   ▼
输出: values, scales (E8M0), amax=None
```

---

## 11. 关键差异总结

### 11.1 NVFP4 vs MXFP4 量化对比

| 特性 | NVFP4 | MXFP4 |
|------|-------|-------|
| 块大小 | 16 | 32 |
| 缩放因子格式 | E4M3 (float8) | E8M0 (uint8) |
| 缩放因子精度 | 有尾数，精度高 | 无尾数，只能表示2的幂 |
| 全局amax | 需要 | 不需要 |
| 反量化公式 | `x * scale * amax / (6 * 448)` | `x * scale` |

### 11.2 FourOverSix实现差异

| 特性 | NVFP4 FourOverSix | MXFP4 FourOverSix |
|------|-------------------|-------------------|
| 方案A缩放 | `max/6` | `max/6` (E8M0) |
| 方案B缩放 | `max/6 * 1.5` | `max/4` (E8M0) |
| 反量化 | 需要 `amax / (6 * 256)` | 直接 `x * scale` |
| 选择函数 | `select_fouroversix()` | `select_fouroversix_mxfp4()` |

### 11.3 为什么MXFP4方案B使用 `max/4` 而非 `max/6 * 1.5`

**原因**：
1. E8M0格式只能表示2的幂（1, 2, 4, 8, ...），不能表示1.5
2. 方案B的目标是让量化后的最大值接近4而非6
3. 直接使用 `max/4` 作为缩放因子，可以确保量化范围覆盖原始数据
4. 与NVFP4的 `max/6 * 1.5 = max/4` 数学等价
