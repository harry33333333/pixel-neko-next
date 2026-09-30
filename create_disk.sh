#!/bin/bash
# create_disk.sh - 创建FAT16硬盘镜像

echo "=== 创建FAT16硬盘镜像 ==="

# 设置镜像大小（默认4GB）
SIZE_MB=${1:-4096}
DISK_FILE="disk.img"

# 检查是否已存在
if [ -f "$DISK_FILE" ]; then
    echo "警告: $DISK_FILE 已存在！"
    read -p "是否覆盖？(y/n): " answer
    if [ "$answer" != "y" ]; then
        echo "取消创建"
        exit 1
    fi
    rm -f "$DISK_FILE"
fi

echo "创建 ${SIZE_MB}MB 硬盘镜像..."
qemu-img create -f raw "$DISK_FILE" ${SIZE_MB}M

echo "格式化为FAT16..."
mkfs.fat -F 16 "$DISK_FILE"

echo "=== 完成 ==="
ls -lh "$DISK_FILE"
echo ""
echo "使用方法:"
echo "  make run          # 使用disk.img启动"
echo "  qemu-system-i386 -cdrom pixel_neko.iso -hda disk.img -m 128M"