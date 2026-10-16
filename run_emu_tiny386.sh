#!/usr/bin/env bash
# run_emu_tiny386.sh - 在 Unix 主机 (Linux / macOS / *BSD 等) 上编译并运行 tiny386
#                      (SDL2 高性能图形界面前端)

set -e

# 定位脚本所在的项目根目录
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$PROJECT_DIR"

HOST_OS="$(uname -s)"
HOST_ARCH="$(uname -m)"

echo "=========================================================="
echo "    tiny386 Native Host (SDL2 GUI) - ${HOST_OS}/${HOST_ARCH}"
echo "=========================================================="

# macOS: Homebrew (/opt/homebrew, /usr/local) 与 MacPorts (/opt/local) 的
#        sdl2-config 可能不在默认 PATH 中，这里主动补上
if [ "$HOST_OS" = "Darwin" ]; then
    for p in /opt/homebrew/bin /usr/local/bin /opt/local/bin; do
        [ -d "$p" ] && case ":$PATH:" in
            *":$p:"*) ;;
            *) PATH="$p:$PATH" ;;
        esac
    done
    export PATH
fi

# 1. 探测 SDL2 开发库：优先 sdl2-config，其次 pkg-config
SDL_CONFIG_CMD=""
if command -v sdl2-config >/dev/null 2>&1; then
    SDL_CONFIG_CMD="sdl2-config"
elif command -v sdl-config >/dev/null 2>&1; then
    SDL_CONFIG_CMD="sdl-config"
elif pkg-config --exists sdl2 2>/dev/null; then
    # Makefile 通过 `${SDL_CONFIG} --cflags/--libs` 调用，pkg-config 兼容该用法
    SDL_CONFIG_CMD="pkg-config sdl2"
else
    echo "[-] 错误: 未找到 SDL2 开发库 (sdl2-config / pkg-config sdl2)。"
    echo "    请先安装，例如："
    echo "      macOS : brew install sdl2"
    echo "      Debian: sudo apt install libsdl2-dev"
    echo "      Fedora: sudo dnf install SDL2-devel"
    exit 1
fi
echo "[+] 使用 SDL2 配置: ${SDL_CONFIG_CMD}"

# 2. 选择合适的 make（BSD 系统的 make 可能不支持本 Makefile，优先 gmake）
MAKE_BIN="make"
if command -v gmake >/dev/null 2>&1; then
    MAKE_BIN="gmake"
fi

# 3. 并行编译核数（跨平台）
JOBS=""
if command -v nproc >/dev/null 2>&1; then
    JOBS="$(nproc)"
elif command -v sysctl >/dev/null 2>&1; then
    JOBS="$(sysctl -n hw.ncpu 2>/dev/null || true)"
elif command -v getconf >/dev/null 2>&1; then
    JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || true)"
fi
[ -n "$JOBS" ] || JOBS=4

# 4. 编译原生 tiny386 (SDL2 前端)
echo "[+] 正在编译原生 tiny386 (SDL2, -j${JOBS})..."
rm -f tiny386
"$MAKE_BIN" SDL_CONFIG="$SDL_CONFIG_CMD" tiny386 -j"$JOBS"

# 5. 解析启动配置文件与参数
#    默认使用 Win95 配置（原生速度足以流畅启动 Win95）；
#    如需运行 DOS 软盘，可传入 conf/win98.ini 等其它配置
INI_FILE="${1:-tiny386_win95.ini}"
shift 1 2>/dev/null || true

if [ ! -f "$INI_FILE" ]; then
    echo "[-] 错误: 配置文件 '$INI_FILE' 不存在！"
    exit 1
fi

echo "[+] 启动原生 tiny386 (配置文件: $INI_FILE, 参数: $@)..."
echo "=========================================================="

exec ./tiny386 "$INI_FILE" "$@"