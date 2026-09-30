#!/bin/bash
# build_counter.sh - 每次编译递增 build number

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

VERSION_FILE="$ROOT_DIR/include/version.h"
BUILD_FILE="$SCRIPT_DIR/build_number.txt"

# 读取当前 build 值
if [ -f "$BUILD_FILE" ]; then
    BUILD_NUM=$(cat "$BUILD_FILE")
else
    BUILD_NUM=1501
fi

# 自增
BUILD_NUM=$((BUILD_NUM + 1))

# 保存
echo "$BUILD_NUM" > "$BUILD_FILE"

# 生成 include/version.h
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