# Pixel Neko Next 🐾

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Architecture](https://img.shields.io/badge/Arch-x86--32-blue.svg)](https://en.wikipedia.org/wiki/IA-32)
[![Boot](https://img.shields.io/badge/Boot-Multiboot%20%2F%20GRUB-green.svg)](https://www.gnu.org/software/grub/)

**Pixel Neko Next** 是一个轻量级、开箱即用的 x86-32 位原生微桌面操作系统（Bare-metal GUI OS）。系统完全基于 C 语言与 NASM 汇编从零编写，具备高分辨率图形桌面、完整的窗口管理器、FAT16/FAT32 文件系统以及实用的内置应用程序。

---

## 🌟 核心特性

- **🖥️ 高清图形桌面 & VBE 显示**
  - 支持最高 1024×768 / 800×600 / 640×480 @ 16-bit (RGB 565) 高清彩色显示，以及兼容标准的 320×200 Mode 13h。
  - 基于线性帧缓冲区（LFB）与软件双缓冲（Backbuffer），支持 VSYNC 画面垂直同步。
- **🔄 实模式跳转蹦床（Real-mode Trampoline）**
  - 在 32 位保护模式与 16 位实模式间实现无缝动态切换，复用 BIOS INT 10h 动态更改屏幕分辨率，复用 BIOS INT 15h (APM) 实现安全电源关机。
- **🪟 完备的窗口管理器（Window Manager）**
  - 支持窗口拖拽、八向大小缩放、Z-Order 层叠管理与顶置聚焦。
  - 拥有任务栏、开始菜单、系统信息悬浮条、桌面图标自由拖动与双击启动。
- **💾 存储与文件系统支持**
  - **IDE / ATA 驱动**：支持主/从盘检测、PIO 模式扇区读写与缓存冲刷（Cache Flush）。
  - **ATAPI 支持**：支持光驱设备识别与 ATAPI (READ 12) 扇区读取。
  - **FAT16 / FAT32 文件系统**：支持根目录及子目录遍历、文件查找、新建、读取、写入持久化保存、重命名与删除。
- **📱 丰富的内置应用程序**
  - **文本编辑器（Editor）**：支持文件打开、键盘光标导航、多行编辑与磁盘回写保存。
  - **计算器（Calculator）**：四则运算，拟物化 3D 浮雕按钮与即时计算。
  - **控制面板（Control Panel）**：动态分辨率切换（带 10 秒无操作安全回滚机制）、鼠标灵敏度调节、系统 CPU / 内存信息展示与关机确认。
  - **文件管理器（File Manager）**：直观浏览驱动器分区目录，支持双击打开关联文件。
- **🧠 内存管理**
  - 基于 Multiboot mmap 解析的物理页框管理器（PMM Bitmap），支持 4KB 页级分配与回收。

---

## 📁 项目目录结构

```text
├── boot/
│   ├── multiboot.asm     # GRUB Multiboot 协议入口与 GDT 初始化
│   └── vbe.asm           # 实模式蹦床（切换分辨率、APM 关机）
├── kernel.c              # 内核主入口、显示模式设定与事件循环
├── wm.c / wm.h           # 窗口管理器核心（图层、拖拽、事件处理、任务栏）
├── mm.c / mm.h           # 物理内存管理（PMM 位图分配器、Multiboot mmap 解析）
├── disk.c / disk.h       # ATA / ATAPI 硬盘与光盘驱动
├── fat.c / fat.h         # FAT16 / FAT32 文件系统驱动
├── keyboard.c / .h       # PS/2 键盘控制器与按键缓冲队列
├── mouse.c / mouse.h     # PS/2 鼠标驱动与数据包解析
├── font.c / font.h       # ASCII 8x16 字体渲染
├── calc.c / calc.h       # 内置计算器应用
├── editor.c / editor.h   # 内置文本编辑器应用
├── cpanel.c / cpanel.h   # 内置控制面板应用
├── filemanager.c / .h    # 内置文件管理器应用
├── link_grub.ld          # 链接脚本
├── Makefile              # 编译与构建规则
└── LICENSE               # MIT 开源协议
```

---

## 🛠️ 构建与运行环境

### 依赖项
- **GCC**（支持 `-m32` 交叉编译）
- **NASM**（汇编器）
- **GNU ld**
- **grub-mkrescue** 与 **xorriso**（用于生成可启动 ISO）
- **QEMU**（推荐 `qemu-system-i386` 用于本地模拟运行）

### 编译与启动

1. **编译生成可启动 ISO 镜像**
   ```bash
   make
   ```
   编译完成后会在根目录生成 `pixel_neko_build_*.iso`。

2. **在 QEMU 虚拟机中一键运行**
   ```bash
   make run
   ```

3. **GDB 远程调试模式**
   ```bash
   make debug
   ```

4. **清理构建生成物**
   ```bash
   make clean
   ```

---

## 📜 开源协议

本项目采用 [MIT 许可证](LICENSE) 开源。欢迎交流学习与改进！
