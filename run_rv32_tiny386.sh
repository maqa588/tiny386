#!/bin/bash
# run_rv32_tiny386.sh - 一键拉起 qemu-riscv32 运行 tiny386 与宿主机 SDL2 渲染窗口

set -e

# 定位脚本所在的项目根目录
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$PROJECT_DIR"

FIFO_VGA="/tmp/tiny386_vga"
FIFO_KBD="/tmp/tiny386_kbd"
SHM_FB="/dev/shm/tiny386_fb"

echo "=========================================================="
echo "    tiny386 RISC-V 32 (ESP32-P4 Core) + SDL2 Viewer"
echo "=========================================================="

# 1. 检查 SDL2 查看器是否已编译
if [ ! -f "tools/sdl2_viewer" ]; then
    echo "[+] 正在编译宿主机 SDL2/OpenGL 渲染前端 (tools/sdl2_viewer)..."
    gcc -O2 tools/sdl2_viewer.c -lSDL2 -lGL -o tools/sdl2_viewer
fi

# 2. 检查并增量构建 tiny386_rv32
export PATH="/home/maqa/.espressif/tools/riscv32-esp-elf/esp-15.2.0_20251204/riscv32-esp-elf/bin:$PATH"
echo "[+] 检查/增量编译 tiny386_rv32..."
make -f Makefile.rv32 -j4

# 3. 检查 qemu-riscv32 是否可用
if ! command -v qemu-riscv32 &> /dev/null; then
    echo "[-] 错误: 未在 PATH 中找到 qemu-riscv32，请先安装 qemu-user"
    exit 1
fi

# 4. 创建零拷贝共享内存与交互管道
rm -f "$FIFO_VGA" "$FIFO_KBD" "$SHM_FB"
mkfifo "$FIFO_VGA" "$FIFO_KBD"
touch "$SHM_FB"
chmod 666 "$SHM_FB" 2>/dev/null || true

# 清理钩子：退出时自动杀掉后台查看器与 QEMU，并删除 FIFO 管道和共享内存
VIEWER_PID=""
QEMU_PID=""
CLEANED=0

cleanup() {
    if [ "$CLEANED" = 1 ]; then return; fi
    CLEANED=1
    echo ""
    echo "[*] 正在退出并清理资源..."
    if [ -n "$QEMU_PID" ] && kill -0 "$QEMU_PID" 2>/dev/null; then
        kill -TERM "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    if [ -n "$VIEWER_PID" ] && kill -0 "$VIEWER_PID" 2>/dev/null; then
        kill "$VIEWER_PID" 2>/dev/null || true
        wait "$VIEWER_PID" 2>/dev/null || true
    fi
    rm -f "$FIFO_VGA" "$FIFO_KBD" "$SHM_FB"
    echo "[*] 完成退出。"
}

# 收到 Ctrl+C / TERM 时先清理再退出（否则 shell 会等前台子进程结束才执行 trap）
on_signal() {
    cleanup
    exit 130
}
trap on_signal INT TERM
trap cleanup EXIT

# 5. 后台拉起 SDL2 显示前端
echo "[+] 启动宿主机 SDL2 显示窗口 (800x480)..."
./tools/sdl2_viewer &
VIEWER_PID=$!

# 等待管道初始化完毕
sleep 0.5

# 6. 使用 qemu-riscv32 启动虚拟机核心（放到后台，便于脚本在 Ctrl+C 时主动终止它）
#    默认使用 Win95 配置（与 run_amd64_tiny386.sh 保持一致）；
#    如需运行 DOS 软盘，可传入 tiny386_pc.ini
INI_FILE="${1:-tiny386_win95.ini}"
shift 1 2>/dev/null || true

if [ ! -f "$INI_FILE" ]; then
    echo "[-] 错误: 配置文件 '$INI_FILE' 不存在！"
    exit 1
fi

echo "[+] 启动 QEMU RISC-V 32 模拟器 (执行配置文件: $INI_FILE $@)..."
echo "=========================================================="

qemu-riscv32 ./tiny386_rv32 "$INI_FILE" "$@" &
QEMU_PID=$!

RET=0
wait "$QEMU_PID" || RET=$?
exit "$RET"
