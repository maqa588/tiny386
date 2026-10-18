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

# 1. 检测并补齐依赖：SDL2 开发库 + libslirp（guest 网络后端）
#    自动识别发行版/包管理器，只安装缺失的包；安装失败不中断，仅提示手动安装
have_sdl2=n
if command -v sdl2-config >/dev/null 2>&1 || command -v sdl-config >/dev/null 2>&1 \
    || pkg-config --exists sdl2 2>/dev/null; then
    have_sdl2=y
fi
# pkg-config 模块名在不同发行版下可能是 "libslirp" 或 "slirp"
have_slirp=n
if pkg-config --exists libslirp 2>/dev/null || pkg-config --exists slirp 2>/dev/null; then
    have_slirp=y
fi

# 识别包管理器，并映射各发行版的包名
PKG_MGR=""
PKG_SDL2=""
PKG_SLIRP=""
if [ "$HOST_OS" = "Darwin" ]; then
    if command -v brew >/dev/null 2>&1; then PKG_MGR="brew"; fi
    PKG_SDL2="sdl2"
    PKG_SLIRP="slirp"
elif command -v pacman >/dev/null 2>&1; then
    PKG_MGR="pacman"; PKG_SDL2="sdl2"; PKG_SLIRP="libslirp"
elif command -v apt-get >/dev/null 2>&1; then
    PKG_MGR="apt-get"; PKG_SDL2="libsdl2-dev"; PKG_SLIRP="libslirp-dev"
elif command -v dnf >/dev/null 2>&1; then
    PKG_MGR="dnf"; PKG_SDL2="SDL2-devel"; PKG_SLIRP="libslirp-devel"
elif command -v yum >/dev/null 2>&1; then
    PKG_MGR="yum"; PKG_SDL2="SDL2-devel"; PKG_SLIRP="libslirp-devel"
elif command -v zypper >/dev/null 2>&1; then
    PKG_MGR="zypper"; PKG_SDL2="libSDL2-devel"; PKG_SLIRP="libslirp-devel"
elif command -v apk >/dev/null 2>&1; then
    PKG_MGR="apk"; PKG_SDL2="sdl2-dev"; PKG_SLIRP="libslirp-dev"
fi

MISSING=""
[ "$have_sdl2" = y ] || MISSING="$MISSING $PKG_SDL2"
[ "$have_slirp" = y ] || MISSING="$MISSING $PKG_SLIRP"
MISSING="${MISSING# }"

if [ -n "$MISSING" ]; then
    if [ -z "$PKG_MGR" ]; then
        echo "[-] 缺少依赖: ${MISSING}，且未识别到受支持的包管理器，请手动安装："
        echo "      Debian/Ubuntu : sudo apt install libsdl2-dev libslirp-dev"
        echo "      Fedora/RHEL   : sudo dnf install SDL2-devel libslirp-devel"
        echo "      Arch          : sudo pacman -S sdl2 libslirp"
        echo "      openSUSE      : sudo zypper install libSDL2-devel libslirp-devel"
        echo "      macOS         : brew install sdl2 slirp"
    else
        echo "[+] 缺少依赖: ${MISSING}，正在使用 ${PKG_MGR} 安装..."
        SUDO=""
        if [ "$(id -u)" -ne 0 ] && command -v sudo >/dev/null 2>&1; then
            SUDO="sudo"
        fi
        { case "$PKG_MGR" in
            brew)    brew install ${MISSING} ;;
            pacman)  ${SUDO} pacman -Sy --needed --noconfirm ${MISSING} ;;
            apt-get) ${SUDO} apt-get update && ${SUDO} apt-get install -y ${MISSING} ;;
            dnf|yum) ${SUDO} ${PKG_MGR} install -y ${MISSING} ;;
            zypper)  ${SUDO} zypper --non-interactive install ${MISSING} ;;
            apk)     ${SUDO} apk add --no-cache ${MISSING} ;;
        esac; } || echo "[-] 依赖安装失败，请手动安装: ${MISSING}"
    fi
fi

# 1.1 探测 SDL2 开发库配置命令：优先 sdl2-config，其次 pkg-config
SDL_CONFIG_CMD=""
if command -v sdl2-config >/dev/null 2>&1; then
    SDL_CONFIG_CMD="sdl2-config"
elif command -v sdl-config >/dev/null 2>&1; then
    SDL_CONFIG_CMD="sdl-config"
elif pkg-config --exists sdl2 2>/dev/null; then
    # Makefile 通过 `${SDL_CONFIG} --cflags/--libs` 调用，pkg-config 兼容该用法
    SDL_CONFIG_CMD="pkg-config sdl2"
else
    echo "[-] 错误: 仍找不到 SDL2 开发库 (sdl2-config / pkg-config sdl2)。"
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
# 先清理旧目标文件：编译开关（例如是否启用 slirp 网络后端）变化时 make 不会
# 自动重建这些对象，会导致实际使用的网络后端与本次编译选项不一致
"$MAKE_BIN" SDL_CONFIG="$SDL_CONFIG_CMD" clean
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