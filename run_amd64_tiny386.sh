#!/bin/bash
# run_amd64_tiny386.sh - 原生 x86_64 运行 tiny386 (SDL2 高性能图形界面)

set -e

# 定位脚本所在的项目根目录
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$PROJECT_DIR"

echo "=========================================================="
echo "    tiny386 Native x86_64 (Host SDL2 GUI)"
echo "=========================================================="

# 1. 检查 SDL2 是否可用
if ! command -v sdl2-config &> /dev/null && ! pkg-config --exists sdl2 2>/dev/null; then
    echo "[-] 警告: 未找到 sdl2-config 或 libsdl2 开发库，请确认已安装 libsdl2-dev"
fi

# 2. 编译原生 tiny386 (USE_SDL=y)
#    注意: rawdraw 与 SDL 两个前端默认都输出为 "tiny386"，先删掉旧产物
#    以保证链接的确是基于 SDL2 的版本。
echo "[+] 正在编译原生 tiny386 (USE_SDL=y)..."
rm -f tiny386
make USE_SDL=y tiny386 -j$(nproc 2>/dev/null || echo 4)

# 3. 解析启动配置文件与参数
#    默认使用 Win95 配置（原生速度足以流畅启动 Win95）；
#    如需运行 DOS 软盘，可传入 tiny386_pc.ini
INI_FILE="${1:-tiny386_win95.ini}"
shift 1 2>/dev/null || true

if [ ! -f "$INI_FILE" ]; then
    echo "[-] 错误: 配置文件 '$INI_FILE' 不存在！"
    exit 1
fi

echo "[+] 启动原生 tiny386 (配置文件: $INI_FILE, 参数: $@)..."
echo "=========================================================="

exec ./tiny386 "$INI_FILE" "$@"
