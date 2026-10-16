// Board support for ESP32-S31 (esp32s31) + WROOM-3 style module.
// Requires ESP-IDF v6.1 (esp32s31 is a PREVIEW target in this tree).
//
// NOTE: ESP32-S31 has no public reference board yet, so every GPIO below is a
// REFERENCE VALUE chosen only to avoid the SoC's reserved/invalid pins and the
// ESP32-S31 EMAC (RGMII) default pins.  *** They MUST be adjusted to match the
// actual PCB. ***  All board-specific pins are grouped here on purpose so that
// only this file needs editing.
//
// ESP32-S31 reserved / invalid GPIOs: 29 and 41 are not bonded out.
// ESP32-S31 EMAC RGMII default pins (used by USE_ETH below):
//   mdc=5, mdio=6, rxc=14, txc=13, tx_ctl=12, txd0..3=8..11,
//   rx_ctl=15, rxd0..3=19/18/17/16

#define BUILD_ESP32

// S31 WROOM-3 modules ship with up to 32MB PSRAM (octal).  26MB is handed to
// the emulator (16MB Guest RAM + 2MB VGA VRAM + ROM/devices); the remainder is
// left for FreeRTOS heaps, DMA and the JIT buffer.  Adjust if the module has
// less PSRAM (esp_main.c clamps to the actually-detected free PSRAM anyway).
#define PSRAM_ALLOC_LEN (26 * 1024 * 1024)

// XXX: ld reports "error: Total discarded sections size is X bytes"
//#define IRAM_ATTR_CPU_EXEC1 IRAM_ATTR
#define IRAM_ATTR_CPU_EXEC1

#define BPP 16
#define FULL_UPDATE
#define USE_PPA_2D
#define USE_LCD_RGB
#define LCD_WIDTH 800
#define LCD_HEIGHT 480

/* -------------------------------------------------------------------------
 * RGB (RGB565, 16-bit parallel) panel  --  REFERENCE PINS, EDIT FOR YOUR PCB
 * ------------------------------------------------------------------------- */
#define LCD_RGB_PCLK  45
#define LCD_RGB_HSYNC 46
#define LCD_RGB_VSYNC 47
#define LCD_RGB_DE    48
#define LCD_RGB_DISP  49
#define LCD_RGB_BL    50

#define LCD_RGB_D0  27
#define LCD_RGB_D1  28
#define LCD_RGB_D2  30
#define LCD_RGB_D3  31
#define LCD_RGB_D4  32
#define LCD_RGB_D5  33
#define LCD_RGB_D6  34
#define LCD_RGB_D7  35
#define LCD_RGB_D8  36
#define LCD_RGB_D9  37
#define LCD_RGB_D10 38
#define LCD_RGB_D11 39
#define LCD_RGB_D12 40
#define LCD_RGB_D13 42
#define LCD_RGB_D14 43
#define LCD_RGB_D15 44

/* 800x480 @ ~60Hz RGB565 panel timing -- REFERENCE VALUES, EDIT FOR YOUR PCB.
 * pclk = (800 + 46 + 88 + 40) * (480 + 3 + 32 + 13) * 60 ~= 31 MHz */
#define LCD_PCLK_HZ           31000000
#define LCD_HSYNC_PULSE_WIDTH 46
#define LCD_HSYNC_BACK_PORCH  88
#define LCD_HSYNC_FRONT_PORCH 40
#define LCD_VSYNC_PULSE_WIDTH 3
#define LCD_VSYNC_BACK_PORCH  32
#define LCD_VSYNC_FRONT_PORCH 13

/* Backlight PWM (LEDC) -- channel/timer are SoC-internal, pin is LCD_RGB_BL. */
#define LCD_BL_LEDC_CHANNEL   1
#define LCD_BL_LEDC_TIMER     1
#define LCD_BL_LEDC_FREQ_HZ   20000

/* -------------------------------------------------------------------------
 * SD card (SDMMC, 4-bit)  --  REFERENCE PINS, EDIT FOR YOUR PCB
 * ------------------------------------------------------------------------- */
#define SD_CLK 51
#define SD_CMD 52
#define SD_D0  53
#define SD_D1  54
#define SD_D2  55
#define SD_D3  56

/* -------------------------------------------------------------------------
 * I2S audio output  --  REFERENCE PINS, EDIT FOR YOUR PCB.
 * S31 has no ES8311 on-board codec, so USE_ES8311 is intentionally NOT set.
 * ------------------------------------------------------------------------- */
#define MIXER_BUF_LEN 512
#define I2S_MCLK 57
#define I2S_BCLK 58
#define I2S_WS   59
#define I2S_DOUT 60
#define I2S_NUM  0

/* -------------------------------------------------------------------------
 * Ethernet: use the ESP-IDF ESP32-S31 EMAC default configuration (RGMII).
 * See esp_eth_mac_esp.h -> ETH_ESP32_EMAC_DEFAULT_CONFIG() for the exact pins.
 * Only the PHY address is board-specific here.
 * ------------------------------------------------------------------------- */
#define USE_ETH
#define ETH_USE_IDF_DEFAULT
#define ETH_PHY_ADDR 1

// S31 uses the on-chip WiFi (native esp_wifi), so USE_WIFI is set.  Do NOT
// enable USE_HOSTED_WIFI: that is only for ESP32-P4 boards with an external
// ESP32-C6 companion, and the default esp_wifi_remote/esp_hosted components
// (and their IDF 6.0 compatibility patch) are no longer part of this tree.
#define USE_WIFI