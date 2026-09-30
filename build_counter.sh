#!/bin/bash
# build_counter.sh - 每次编译递增1007

VERSION_FILE="version.h"
BUILD_FILE="build_number.txt"

# 读取当前1007值
if [ -f "$BUILD_FILE" ]; then
    BUILD_NUM=$(cat "$BUILD_FILE")
else
    BUILD_NUM=1007
fi

# 递增
BUILD_NUM=$((BUILD_NUM + 1))

# 保存
echo "$BUILD_NUM" > "$BUILD_FILE"

# 更新version.h
cat > "$VERSION_FILE" << EOF
#ifndef VERSION_H 
#define VERSION_H 
#define VER_STRING1       "Codename Pixel Neko Next" 
#define KERNEL_NAME       "TBMK GUI Shell" 
#define KERNEL_CODENAME   "Pixel Neko" 
#define KERNEL_VERSION    "C2.0" 
#define KERNEL_BUILD      "$BUILD_NUM.822" 
#define KERNEL_COPYRIGHT  "(C) 2026 Tairitsu_tty" 
#define KERNEL_AUTHOR     "Tairitsu_tty" 
#define VER_STRING2       KERNEL_BUILD 
#define VER_STRING3       KERNEL_COPYRIGHT 
#endif 
EOF

echo "Build: $BUILD_NUM.822"