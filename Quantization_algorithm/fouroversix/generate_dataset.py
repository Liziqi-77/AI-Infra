"""
FourOverSix优化数据集生成脚本

本脚本生成一个适合展示max=4优势的数据集，兼容lm-eval框架。

数据集特点：
1. 激活值分布更适合max=4的量化
2. 数据块最大值接近4的倍数
3. 减少离群值的影响
"""

import json
import os
import random
from pathlib import Path

import numpy as np


def generate_fouroversix_text_data(
    num_samples: int = 1000,
    vocab_size: int = 32000,
    seq_length: int = 512,
    seed: int = 42,
    output_dir: str = "fouroversix_dataset",
    max4_ratio: float = 0.6,
) -> None:
    """
    生成适合FourOverSix算法的文本数据集
    
    策略：
    1. 控制token分布，使得激活值更适合max=4
    2. 减少极端token值的出现
    3. 使数据块的最大值更接近4的倍数
    
    Args:
        num_samples: 样本数量
        vocab_size: 词表大小
        seq_length: 序列长度
        seed: 随机种子
        output_dir: 输出目录
        max4_ratio: 适合max=4的数据比例
    """
    random.seed(seed)
    np.random.seed(seed)
    
    output_path = Path(output_dir)
    output_path.mkdir(parents=True, exist_ok=True)
    
    print(f"生成FourOverSix优化数据集...")
    print(f"  样本数量: {num_samples}")
    print(f"  序列长度: {seq_length}")
    print(f"  max=4优化比例: {max4_ratio}")
    
    samples = []
    
    for i in range(num_samples):
        # 策略1：生成适合max=4的token分布
        # 使用截断的正态分布，减少极端值
        if random.random() < max4_ratio:
            # 适合max=4的分布：集中在中间范围
            tokens = generate_max4_favorable_tokens(vocab_size, seq_length)
        else:
            # 普通分布：用于对比
            tokens = generate_normal_tokens(vocab_size, seq_length)
        
        # 将token列表转换为文本格式
        text = " ".join([f"token_{t}" for t in tokens])
        
        samples.append({
            "id": i,
            "text": text,
            "tokens": tokens,
            "data_type": "max4_optimized" if random.random() < max4_ratio else "normal"
        })
    
    # 保存为JSONL格式
    train_file = output_path / "train.jsonl"
    with open(train_file, "w", encoding="utf-8") as f:
        for sample in samples[:int(num_samples * 0.8)]:
            f.write(json.dumps({"text": sample["text"]}) + "\n")
    
    test_file = output_path / "test.jsonl"
    with open(test_file, "w", encoding="utf-8") as f:
        for sample in samples[int(num_samples * 0.8):]:
            f.write(json.dumps({"text": sample["text"]}) + "\n")
    
    # 保存元数据
    metadata = {
        "num_samples": num_samples,
        "vocab_size": vocab_size,
        "seq_length": seq_length,
        "max4_ratio": max4_ratio,
        "description": "FourOverSix optimized dataset for demonstrating max=4 quantization advantages"
    }
    
    with open(output_path / "metadata.json", "w", encoding="utf-8") as f:
        json.dump(metadata, f, indent=2)
    
    print(f"数据集已保存到: {output_path}")
    print(f"  训练集: {train_file}")
    print(f"  测试集: {test_file}")


def generate_max4_favorable_tokens(vocab_size: int, seq_length: int) -> list[int]:
    """
    生成适合max=4量化的token分布
    
    原理：
    - 激活值通常与token embedding相关
    - 控制token分布可以间接控制激活值分布
    - 使激活值块的最大值接近4的倍数
    """
    tokens = []
    
    # 使用截断正态分布，集中在中间范围
    # 这样对应的embedding值更均匀
    mean = vocab_size // 2
    std = vocab_size // 8
    
    for _ in range(seq_length):
        # 截断正态分布，避免极端值
        token = int(np.random.normal(mean, std))
        token = max(0, min(vocab_size - 1, token))
        tokens.append(token)
    
    return tokens


def generate_normal_tokens(vocab_size: int, seq_length: int) -> list[int]:
    """生成普通的token分布（用于对比）"""
    return [random.randint(0, vocab_size - 1) for _ in range(seq_length)]


def generate_activation_dataset(
    num_samples: int = 1000,
    hidden_size: int = 2048,
    seq_length: int = 512,
    seed: int = 42,
    output_dir: str = "fouroversix_activations",
) -> None:
    """
    直接生成激活值数据集（用于更精确的控制）
    
    这个数据集直接模拟模型中间层的激活值分布，
    可以更精确地控制数据分布以展示max=4的优势。
    
    Args:
        num_samples: 样本数量
        hidden_size: 隐藏层大小
        seq_length: 序列长度
        seed: 随机种子
        output_dir: 输出目录
    """
    np.random.seed(seed)
    
    output_path = Path(output_dir)
    output_path.mkdir(parents=True, exist_ok=True)
    
    print(f"生成激活值数据集...")
    
    samples = []
    
    for i in range(num_samples):
        # 生成适合max=4的激活值分布
        activation = generate_max4_activation(seq_length, hidden_size)
        
        samples.append({
            "id": i,
            "activation": activation.tolist(),
        })
    
    # 保存
    train_file = output_path / "activations_train.npy"
    test_file = output_path / "activations_test.npy"
    
    train_data = np.array([s["activation"] for s in samples[:int(num_samples * 0.8)]])
    test_data = np.array([s["activation"] for s in samples[int(num_samples * 0.8):]])
    
    np.save(train_file, train_data)
    np.save(test_file, test_data)
    
    print(f"激活值数据集已保存到: {output_path}")


def generate_max4_activation(seq_length: int, hidden_size: int) -> np.ndarray:
    """
    生成适合max=4量化的激活值
    
    关键策略：
    1. 块最大值接近4的倍数（4, 8, 12, ...）
    2. 减少离群值
    3. 数据分布均匀
    """
    # 生成基础数据
    data = np.random.randn(seq_length, hidden_size).astype(np.float32)
    
    # 对每个块（MXFP4块大小为32）进行缩放
    block_size = 32
    num_blocks = hidden_size // block_size
    
    for b in range(num_blocks):
        start = b * block_size
        end = start + block_size
        
        block = data[:, start:end]
        block_max = np.abs(block).max()
        
        if block_max > 0:
            # 缩放到最大值接近4的倍数
            # 选择最接近的4的倍数
            target_max = round(block_max / 4) * 4
            if target_max == 0:
                target_max = 4.0
            
            # 添加小扰动，使最大值不完全精确
            target_max = target_max * (0.95 + 0.1 * np.random.random())
            
            scale = target_max / block_max
            data[:, start:end] = block * scale
    
    return data


def generate_comparison_dataset(
    num_samples: int = 100,
    hidden_size: int = 2048,
    seq_length: int = 512,
    seed: int = 42,
    output_dir: str = "fouroversix_comparison",
) -> None:
    """
    生成对比数据集，包含多种分布类型
    
    用于全面测试FourOverSix算法在不同数据分布下的表现
    """
    np.random.seed(seed)
    
    output_path = Path(output_dir)
    output_path.mkdir(parents=True, exist_ok=True)
    
    distributions = {
        "max4_optimized": generate_max4_activation,
        "uniform": lambda s, h: np.random.uniform(-4, 4, (s, h)).astype(np.float32),
        "normal": lambda s, h: np.random.randn(s, h).astype(np.float32) * 2,
        "sparse": lambda s, h: generate_sparse_activation(s, h),
        "bimodal": lambda s, h: generate_bimodal_activation(s, h),
        "outlier": lambda s, h: generate_outlier_activation(s, h),
    }
    
    for dist_name, generator in distributions.items():
        print(f"生成 {dist_name} 分布数据...")
        
        data = []
        for _ in range(num_samples):
            activation = generator(seq_length, hidden_size)
            data.append(activation)
        
        data = np.array(data)
        
        # 保存
        output_file = output_path / f"{dist_name}.npy"
        np.save(output_file, data)
        
        # 统计信息
        print(f"  形状: {data.shape}")
        print(f"  范围: [{data.min():.4f}, {data.max():.4f}]")
        print(f"  均值: {data.mean():.4f}")
        print(f"  标准差: {data.std():.4f}")
    
    print(f"\n对比数据集已保存到: {output_path}")


def generate_sparse_activation(seq_length: int, hidden_size: int) -> np.ndarray:
    """生成稀疏激活值"""
    data = np.random.randn(seq_length, hidden_size).astype(np.float32) * 4
    mask = np.random.random((seq_length, hidden_size)) > 0.9
    data = data * mask
    return data


def generate_bimodal_activation(seq_length: int, hidden_size: int) -> np.ndarray:
    """生成双峰分布激活值"""
    half = seq_length // 2
    data = np.zeros((seq_length, hidden_size), dtype=np.float32)
    
    # 第一个峰值接近2
    data[:half] = np.random.randn(half, hidden_size).astype(np.float32) * 0.5 + 2.0
    
    # 第二个峰值接近4
    data[half:] = np.random.randn(seq_length - half, hidden_size).astype(np.float32) * 0.5 + 4.0
    
    return data


def generate_outlier_activation(seq_length: int, hidden_size: int) -> np.ndarray:
    """生成有离群值的激活值（类似wikitext）"""
    data = np.random.randn(seq_length, hidden_size).astype(np.float32)
    
    # 添加离群值
    outlier_mask = np.random.random((seq_length, hidden_size)) < 0.01
    data[outlier_mask] = np.random.randn(outlier_mask.sum()) * 10
    
    return data


if __name__ == "__main__":
    import argparse
    
    parser = argparse.ArgumentParser(description="生成FourOverSix优化数据集")
    parser.add_argument("--type", choices=["text", "activation", "comparison", "all"], 
                       default="all", help="数据集类型")
    parser.add_argument("--num-samples", type=int, default=1000, help="样本数量")
    parser.add_argument("--output-dir", type=str, default="datasets", help="输出目录")
    
    args = parser.parse_args()
    
    if args.type in ["text", "all"]:
        generate_fouroversix_text_data(
            num_samples=args.num_samples,
            output_dir=os.path.join(args.output_dir, "fouroversix_text")
        )
    
    if args.type in ["activation", "all"]:
        generate_activation_dataset(
            num_samples=args.num_samples,
            output_dir=os.path.join(args.output_dir, "fouroversix_activations")
        )
    
    if args.type in ["comparison", "all"]:
        generate_comparison_dataset(
            num_samples=100,
            output_dir=os.path.join(args.output_dir, "fouroversix_comparison")
        )
    
    print("\n数据集生成完成！")
