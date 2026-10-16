#!/bin/bash
# run_esp_emu.sh - 一键使用乐鑫官方 esp-emu 模拟器运行 ESP32-P4 固件进行整机仿真

set -e

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$PROJECT_DIR"

ESP_EMU="/Volumes/maqassd/.espressif/bin/esp-emu"
if [ ! -x "$ESP_EMU" ]; then
    ESP_EMU="$(command -v esp-emu 2>/dev/null || true)"
fi

if [ -z "$ESP_EMU" ] || [ ! -x "$ESP_EMU" ]; then
    echo "[-] 错误: 未找到 esp-emu，请检查 /Volumes/maqassd/.espressif/bin/esp-emu"
    exit 1
fi

FIRMWARE="$PROJECT_DIR/src/esp/build/merged_flash.bin"
ELF="$PROJECT_DIR/src/esp/build/tiny386.elf"

if [ ! -f "$FIRMWARE" ]; then
    echo "[-] 错误: 固件文件 $FIRMWARE 不存在，请先编译并合并固件！"
    exit 1
fi

echo "=========================================================="
echo "    tiny386 ESP32-P4 (Dual RISC-V HP Cores) in esp-emu"
echo "=========================================================="
echo "[+] 启动乐鑫官方 esp-emu 仿真器..."
echo "[+] 芯片: ESP32-P4 | PSRAM: 32MB | 固件: $FIRMWARE"
echo "=========================================================="

"$ESP_EMU" --chip esp32p4 --psram-size 32M --firmware "$FIRMWARE" --elf "$ELF" "$@"
