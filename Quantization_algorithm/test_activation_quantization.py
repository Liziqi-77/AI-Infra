"""
直接测试激活值量化脚本

本脚本直接测试不同分布的激活值在FourOverSix算法下的表现，
不需要依赖lm-eval框架，可以直接在GPU上运行。

使用方法：
    python test_activation_quantization.py --dtype mxfp4 --scale-rule mse
"""

import argparse
import logging
import sys
from pathlib import Path

import numpy as np
import torch

sys.path.insert(0, str(Path(__file__).parent.parent / "src"))

logging.basicConfig(level=logging.INFO, format='%(message)s')

from fouroversix import quantize_to_fp4, QuantizationConfig, DataType, ScaleRule, QuantizeBackend


def print_separator(title: str):
    print("\n" + "=" * 70)
    print(f" {title}")
    print("=" * 70)


def generate_max4_optimized_activation(shape: tuple, device: str = "cuda") -> torch.Tensor:
    """
    生成适合max=4量化的激活值
    
    关键策略：
    1. 块最大值接近4的倍数
    2. 减少离群值
    3. 数据分布均匀
    """
    data = torch.randn(shape, dtype=torch.bfloat16, device=device)
    
    block_size = 32  # MXFP4块大小
    num_blocks = shape[1] // block_size
    
    for b in range(num_blocks):
        start = b * block_size
        end = start + block_size
        
        block = data[:, start:end]
        block_max = block.abs().max()
        
        if block_max > 0:
            # 缩放到最大值接近4的倍数
            target_max = round(block_max.item() / 4) * 4
            if target_max == 0:
                target_max = 4.0
            
            # 添加小扰动
            target_max = target_max * (0.95 + 0.1 * torch.rand(1, device=device).item())
            
            scale = target_max / block_max
            data[:, start:end] = block * scale
    
    return data


def generate_uniform_activation(shape: tuple, device: str = "cuda") -> torch.Tensor:
    """生成均匀分布激活值"""
    return torch.rand(shape, dtype=torch.bfloat16, device=device) * 8 - 4


def generate_normal_activation(shape: tuple, device: str = "cuda") -> torch.Tensor:
    """生成正态分布激活值"""
    return torch.randn(shape, dtype=torch.bfloat16, device=device) * 2


def generate_sparse_activation(shape: tuple, device: str = "cuda") -> torch.Tensor:
    """生成稀疏激活值"""
    data = torch.randn(shape, dtype=torch.bfloat16, device=device) * 4
    mask = torch.rand(shape, device=device) > 0.9
    return data * mask


def generate_bimodal_activation(shape: tuple, device: str = "cuda") -> torch.Tensor:
    """生成双峰分布激活值"""
    half = shape[0] // 2
    data = torch.zeros(shape, dtype=torch.bfloat16, device=device)
    
    # 第一个峰值接近2
    data[:half] = torch.randn(half, shape[1], dtype=torch.bfloat16, device=device) * 0.5 + 2.0
    
    # 第二个峰值接近4
    data[half:] = torch.randn(shape[0] - half, shape[1], dtype=torch.bfloat16, device=device) * 0.5 + 4.0
    
    return data


def generate_outlier_activation(shape: tuple, device: str = "cuda") -> torch.Tensor:
    """生成有离群值的激活值（类似wikitext）"""
    data = torch.randn(shape, dtype=torch.bfloat16, device=device)
    
    # 添加离群值
    outlier_mask = torch.rand(shape, device=device) < 0.01
    data[outlier_mask] = torch.randn(outlier_mask.sum().item(), dtype=torch.bfloat16, device=device) * 10
    
    return data


def generate_exact_max4_activation(shape: tuple, device: str = "cuda") -> torch.Tensor:
    """生成最大值恰好为4的激活值（最有利于max=4）"""
    block_size = 32
    num_blocks = shape[0] * shape[1] // block_size
    
    data_blocks = torch.zeros(num_blocks, block_size, dtype=torch.bfloat16, device=device)
    
    for i in range(num_blocks):
        block = torch.randn(block_size, dtype=torch.bfloat16, device=device)
        block_max = block.abs().max()
        if block_max > 0:
            block = block * (4.0 / block_max)
        data_blocks[i] = block
    
    return data_blocks.reshape(shape[0], shape[1])


def test_quantization(
    data: torch.Tensor,
    name: str,
    dtype: DataType,
    scale_rule: ScaleRule,
    log_enabled: bool = True,
) -> dict:
    """测试量化效果"""
    print_separator(f"测试: {name}")
    
    # 数据统计
    print(f"数据形状: {data.shape}")
    print(f"数据范围: [{data.min().item():.4f}, {data.max().item():.4f}]")
    print(f"数据均值: {data.mean().item():.4f}")
    print(f"数据标准差: {data.std().item():.4f}")
    
    # 执行量化
    config = QuantizationConfig(
        dtype=dtype,
        scale_rule=scale_rule,
        backend=QuantizeBackend.pytorch,
        log_fouroversix=log_enabled,
    )
    
    quantized = quantize_to_fp4(data, config)
    
    # 计算量化误差
    dequantized = quantized.dequantize(torch.bfloat16)
    mae = (dequantized - data).abs().mean().item()
    mse = ((dequantized - data) ** 2).mean().item()
    
    print(f"\n量化误差:")
    print(f"  MAE: {mae:.6f}")
    print(f"  MSE: {mse:.6f}")
    
    return {
        "name": name,
        "shape": data.shape,
        "min": data.min().item(),
        "max": data.max().item(),
        "mean": data.mean().item(),
        "std": data.std().item(),
        "mae": mae,
        "mse": mse,
    }


def compare_strategies(
    data: torch.Tensor,
    name: str,
    dtype: DataType,
) -> dict:
    """比较不同策略的量化效果"""
    print_separator(f"策略比较: {name}")
    
    print(f"数据形状: {data.shape}")
    print(f"数据范围: [{data.min().item():.4f}, {data.max().item():.4f}]")
    
    results = {}
    
    # 测试不同策略
    strategies = [
        ("自适应(mse)", ScaleRule.mse, False),
        ("强制max=4", ScaleRule.mse, True),
        ("静态max=6", ScaleRule.static_6, False),
        ("静态max=4", ScaleRule.static_4, False),
    ]
    
    for strategy_name, scale_rule, force_max_4 in strategies:
        config = QuantizationConfig(
            dtype=dtype,
            scale_rule=scale_rule,
            backend=QuantizeBackend.pytorch,
            force_max_4=force_max_4,
            log_fouroversix=(strategy_name == "自适应(mse)"),
        )
        
        quantized = quantize_to_fp4(data.clone(), config)
        dequantized = quantized.dequantize(torch.bfloat16)
        mae = (dequantized - data).abs().mean().item()
        
        results[strategy_name] = mae
        print(f"  {strategy_name}: MAE = {mae:.6f}")
    
    # 找出最优策略
    best_strategy = min(results, key=results.get)
    print(f"\n最优策略: {best_strategy} (MAE = {results[best_strategy]:.6f})")
    
    return results


def main():
    parser = argparse.ArgumentParser(description="测试激活值量化")
    parser.add_argument("--dtype", type=str, default="mxfp4", choices=["mxfp4", "nvfp4"])
    parser.add_argument("--scale-rule", type=str, default="mse", choices=["mse", "mae", "abs_max", "static_6", "static_4"])
    parser.add_argument("--shape", type=str, default="256,256", help="数据形状，如 256,256")
    parser.add_argument("--compare", action="store_true", help="比较不同策略")
    args = parser.parse_args()
    
    dtype = DataType(args.dtype)
    scale_rule = ScaleRule(args.scale_rule)
    shape = tuple(map(int, args.shape.split(",")))
    
    print_separator("FourOverSix激活值量化测试")
    print(f"数据类型: {dtype.value}")
    print(f"缩放规则: {scale_rule.value}")
    print(f"数据形状: {shape}")
    
    # 生成不同分布的数据
    distributions = [
        ("max=4优化", generate_max4_optimized_activation),
        ("均匀分布", generate_uniform_activation),
        ("正态分布", generate_normal_activation),
        ("稀疏分布", generate_sparse_activation),
        ("双峰分布", generate_bimodal_activation),
        ("有离群值", generate_outlier_activation),
        ("最大值恰好为4", generate_exact_max4_activation),
    ]
    
    all_results = []
    
    for name, generator in distributions:
        data = generator(shape)
        
        if args.compare:
            results = compare_strategies(data, name, dtype)
            all_results.append({"name": name, "results": results})
        else:
            result = test_quantization(data, name, dtype, scale_rule)
            all_results.append(result)
    
    # 总结
    print_separator("测试总结")
    
    if args.compare:
        print("\n各分布下的最优策略:")
        for item in all_results:
            best = min(item["results"], key=item["results"].get)
            print(f"  {item['name']}: {best} (MAE = {item['results'][best]:.6f})")
    else:
        print("\n各分布下的量化误差(MAE):")
        for result in all_results:
            print(f"  {result['name']}: {result['mae']:.6f}")


if __name__ == "__main__":
    main()
