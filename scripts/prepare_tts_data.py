#!/usr/bin/env python3
"""
准备 TTS 语音数据文件的脚本

用法:
    python scripts/prepare_tts_data.py [目标路径]

示例:
    python scripts/prepare_tts_data.py D:/
    python scripts/prepare_tts_data.py /Volumes/SDCARD/
"""

import os
import sys
import shutil
from pathlib import Path

def main():
    # 源文件路径
    project_root = Path(__file__).parent.parent
    source_file = project_root / "managed_components/espressif__esp-sr/esp-tts/esp_tts_chinese/esp_tts_voice_data_xiaole.dat"

    if not source_file.exists():
        print(f"❌ 错误: 找不到语音数据文件")
        print(f"   路径: {source_file}")
        print(f"   请确保 ESP-SR 组件已正确安装")
        return 1

    # 检查文件大小
    file_size = source_file.stat().st_size
    size_mb = file_size / (1024 * 1024)
    print(f"✓ 找到语音数据: {source_file.name}")
    print(f"  大小: {size_mb:.1f} MB ({file_size:,} 字节)")

    # 目标路径
    if len(sys.argv) > 1:
        target_dir = Path(sys.argv[1])
    else:
        print("\n请指定目标路径（TF 卡挂载点）:")
        print("  例如: D:\\ 或 /Volumes/SDCARD/")
        target_path = input("\n目标路径: ").strip()
        if not target_path:
            print("已取消")
            return 0
        target_dir = Path(target_path)

    if not target_dir.exists():
        print(f"\n❌ 错误: 目标路径不存在: {target_dir}")
        return 1

    if not target_dir.is_dir():
        print(f"\n❌ 错误: 目标路径不是目录: {target_dir}")
        return 1

    target_file = target_dir / "esp_tts_voice_data_xiaole.dat"

    # 检查目标空间
    try:
        stat = shutil.disk_usage(target_dir)
        free_mb = stat.free / (1024 * 1024)
        if free_mb < size_mb:
            print(f"\n⚠️  警告: 目标空间不足")
            print(f"   需要: {size_mb:.1f} MB")
            print(f"   可用: {free_mb:.1f} MB")
            return 1
    except Exception as e:
        print(f"\n⚠️  无法检查磁盘空间: {e}")

    # 检查是否已存在
    if target_file.exists():
        existing_size = target_file.stat().st_size
        if existing_size == file_size:
            print(f"\n✓ 文件已存在且大小匹配")
            print(f"  {target_file}")
            overwrite = input("\n是否覆盖? (y/N): ").strip().lower()
            if overwrite != 'y':
                print("已取消")
                return 0
        else:
            print(f"\n⚠️  目标文件已存在但大小不匹配")
            print(f"   现有: {existing_size:,} 字节")
            print(f"   源文件: {file_size:,} 字节")
            overwrite = input("\n是否覆盖? (y/N): ").strip().lower()
            if overwrite != 'y':
                print("已取消")
                return 0

    # 复制文件
    print(f"\n正在复制到: {target_file}")
    try:
        shutil.copy2(source_file, target_file)
        print(f"✓ 复制完成")

        # 验证
        copied_size = target_file.stat().st_size
        if copied_size == file_size:
            print(f"✓ 验证通过: 大小匹配")
        else:
            print(f"❌ 验证失败: 大小不匹配")
            print(f"   期望: {file_size:,} 字节")
            print(f"   实际: {copied_size:,} 字节")
            return 1

        print(f"\n成功! 设备启动后语音数据将从以下路径加载:")
        print(f"  /sdcard/esp_tts_voice_data_xiaole.dat")

    except Exception as e:
        print(f"\n❌ 复制失败: {e}")
        return 1

    return 0

if __name__ == "__main__":
    sys.exit(main())
