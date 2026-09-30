#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
项目源码收集脚本
收集 .c/.h/.sh/.py/Makefile 文件到 output.txt
"""

import os
from pathlib import Path
from datetime import datetime

def read_file_with_fallback(file_path):
    """尝试多种编码读取文件"""
    encodings = ['utf-8', 'gbk', 'gb2312', 'gb18030', 'big5', 'latin-1', 'cp1252', 'ascii']
    
    for enc in encodings:
        try:
            with open(file_path, 'r', encoding=enc) as f:
                return f.read()
        except (UnicodeDecodeError, UnicodeError):
            continue
    
    # 最后尝试二进制读取，用 replace 处理无法解码的字符
    try:
        with open(file_path, 'rb') as f:
            raw = f.read()
        return raw.decode('utf-8', errors='replace')
    except:
        return "[无法读取文件]"

def collect_sources(source_dir="."):
    """收集指定目录下的源码文件"""
    
    source_path = Path(source_dir)
    output_file = "output.txt"
    
    # 忽略的目录
    ignore_dirs = {'.git', '__pycache__', 'iso', 'build', 'obj', 'bin', 'dist', '.venv', 'node_modules'}
    # 忽略的文件
    ignore_files = {'output.txt', 'disk.img', 'pixel_neko.iso', 'tbmk_grub.iso'}
    
    target_files = []
    
    # 遍历目录
    for root, dirs, files in os.walk(source_path):
        # 过滤忽略的目录
        dirs[:] = [d for d in dirs if d not in ignore_dirs]
        
        for file in files:
            if file in ignore_files:
                continue
            
            file_path = Path(root) / file
            ext = file_path.suffix.lower()
            
            # 收集 .c, .h, .sh, .py 文件
            if ext in {'.c', '.h', '.sh', '.py'}:
                target_files.append(file_path)
            # 收集 Makefile
            elif file == 'Makefile':
                target_files.append(file_path)
    
    # 排序
    target_files.sort(key=lambda x: str(x).lower())
    
    if not target_files:
        print("未找到任何文件！")
        return
    
    print(f"找到 {len(target_files)} 个文件")
    
    # 写入输出文件
    with open(output_file, 'w', encoding='utf-8') as out:
        # 文件头
        out.write("=" * 80 + "\n")
        out.write("项目源码收集\n")
        out.write(f"生成时间: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n")
        out.write(f"源目录: {source_path.absolute()}\n")
        out.write(f"文件总数: {len(target_files)}\n")
        out.write("=" * 80 + "\n\n")
        
        # 写入每个文件
        for i, file_path in enumerate(target_files, 1):
            rel_path = file_path.relative_to(source_path)
            print(f"处理 ({i}/{len(target_files)}): {rel_path}")
            
            # 文件标记
            out.write("\n" + "*" * 80 + "\n")
            out.write(f"文件 {i}: {rel_path}\n")
            out.write("*" * 80 + "\n\n")
            
            # 读取并写入内容
            content = read_file_with_fallback(file_path)
            out.write(content)
            if content and not content.endswith('\n'):
                out.write('\n')
            
            # 文件结束标记
            out.write("\n" + "-" * 80 + "\n")
        
        # 文件尾
        out.write("\n" + "=" * 80 + "\n")
        out.write("收集完成\n")
        out.write(f"总文件数: {len(target_files)}\n")
        out.write(f"完成时间: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n")
        out.write("=" * 80 + "\n")
    
    print(f"\n✅ 成功生成 {output_file}")
    print(f"   包含 {len(target_files)} 个文件")

if __name__ == "__main__":
    import sys
    
    source_dir = sys.argv[1] if len(sys.argv) > 1 else "."
    collect_sources(source_dir)