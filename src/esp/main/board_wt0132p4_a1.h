// 深圳市启明云端 WT0132P4-A1 核心板 (ESP32-P4), requires esp-idf v6.1
//
// 16 MB NOR flash + 16/32 MB in-package PSRAM, 82-pin castellated core module,
// "ESP32-P4 芯片全引脚引出".  Peripheral wiring below follows the WT0132P4-A1
// carrier schematic (RMII Ethernet + SDMMC TF card + RGB565->ADV7123 VGA +
// I2S->PCM5102A audio).  Peripherals not populated on your carrier can simply be
// commented out.
//
// Pin mapping against the WT0132P4-A1 hardware pin table (module pin numbers):
#define BUILD_ESP32

// 16/32MB in-package PSRAM: 20MB pool for the emulator (16MB guest RAM + 2MB VGA VRAM
// + 2MB headroom); the remaining 12MB is dedicated to 4MB PSRAM JIT buffer, LCD FB, and DMA/system heaps.
#define PSRAM_ALLOC_LEN (20 * 1024 * 1024)

// -------------------------------------------------------------------------
// LP (40 MHz) core standby supervisor.
//
// Syncs with the guest's APM low-power request (Windows 95 Start -> Suspend):
// when SeaBIOS reports "Standby"/"Suspend", Core 0 parks the HP cores in light
// sleep and the LP core keeps the RTC domain alive, waking the HP cores back
// up on its own timer (see pm.c).  Requires the LP core to be enabled in
// sdkconfig.wt0132p4_a1 (CONFIG_ULP_COPROC_ENABLED / _TYPE_LP_CORE).
// -------------------------------------------------------------------------
#define USE_LP_CORE_PM
// LP timer interval, in microseconds: how often the LP core wakes itself to
// check on the parked machine and hand control back to the HP cores.
#define LP_PM_WAKE_PERIOD_US   (2000000)
// Safety net, in microseconds: the guest is always resumed after this long.
// USB HS activity and the optional wake button (below) resume it earlier.
#define LP_PM_MAX_SUSPEND_US   (300000000LL)
// Optional wake button wired to an RTC-capable GPIO.  When defined, standby is
// also left as soon as the pin goes low (active-low momentary switch).
#define LP_PM_WAKE_GPIO        0


// XXX: ld reports "error: Total discarded sections size is X bytes"
//#define IRAM_ATTR_CPU_EXEC1 IRAM_ATTR
#define IRAM_ATTR_CPU_EXEC1

#define BPP 16
#define FULL_UPDATE
#define USE_PPA_2D
#ifndef USE_PPA_2D
#define SWAPXY
#endif

// -------------------------------------------------------------------------
// VGA output: 16-bit RGB565 parallel bus -> ADV7123 video DAC -> DB15 VGA.
// 640x480 @ 60Hz standard VGA timing (25.175 MHz pixel clock).
// -------------------------------------------------------------------------
#define USE_LCD_RGB
#define LCD_WIDTH 640
#define LCD_HEIGHT 480

// esp_lcd RGB data bus bit0..15.  RGB565 is MSB-aligned on the bus:
//   D[4:0]  = Blue  B0..B4  -> ADV7123 B5..B9  (GPIO3..7)
//   D[10:5] = Green G0..G5  -> ADV7123 G4..G9  (GPIO50..54, GPIO2)
//   D[15:11]= Red   R0..R4  -> ADV7123 R5..R9  (GPIO45..49)
#define LCD_RGB_D0  3   // B0  pin 56
#define LCD_RGB_D1  4   // B1  pin 57
#define LCD_RGB_D2  5   // B2  pin 58
#define LCD_RGB_D3  6   // B3  pin 59
#define LCD_RGB_D4  7   // B4  pin 60
#define LCD_RGB_D5  50  // G0  pin 49
#define LCD_RGB_D6  51  // G1  pin 50
#define LCD_RGB_D7  52  // G2  pin 51
#define LCD_RGB_D8  53  // G3  pin 52
#define LCD_RGB_D9  54  // G4  pin 54
#define LCD_RGB_D10 2   // G5  pin 55  (strapping pin)
#define LCD_RGB_D11 45  // R0  pin 44
#define LCD_RGB_D12 46  // R1  pin 45
#define LCD_RGB_D13 47  // R2  pin 46
#define LCD_RGB_D14 48  // R3  pin 47
#define LCD_RGB_D15 49  // R4  pin 48

#define LCD_RGB_PCLK  8  // pin 61  -> ADV7123 CLK
#define LCD_RGB_DE    9  // pin 62  -> ADV7123 /BLANK
#define LCD_RGB_HSYNC 10 // pin 63  -> VGA U4 pin 13
#define LCD_RGB_VSYNC 11 // pin 65  -> VGA U4 pin 14
// The ADV7123 path has neither a panel DISP line nor a backlight PWM.
#define LCD_RGB_DISP  -1
#define LCD_RGB_BL    -1

// 640x480 @ 60Hz standard VGA (VESA DMT): pixel clock 25.175 MHz,
// H: 640 + 16(front) + 96(sync) + 48(back) = 800
// V: 480 + 10(front) +  2(sync) + 33(back) = 525
// => 800 * 525 * 60 = 25.2 MHz.  Sync pulses are active-low (idle high).
#define LCD_PCLK_HZ           25175000
#define LCD_HSYNC_PULSE_WIDTH 96
#define LCD_HSYNC_BACK_PORCH  48
#define LCD_HSYNC_FRONT_PORCH 16
#define LCD_VSYNC_PULSE_WIDTH 2
#define LCD_VSYNC_BACK_PORCH  33
#define LCD_VSYNC_FRONT_PORCH 10
#define LCD_RGB_HSYNC_IDLE_LOW 1
#define LCD_RGB_VSYNC_IDLE_LOW 1

// -------------------------------------------------------------------------
// SD / TF card (SDMMC 4-bit high speed)
// -------------------------------------------------------------------------
#define SD_CLK 43              // pin 40
#define SD_CMD 44              // pin 43
#define SD_D0  39              // pin 36
#define SD_D1  40              // pin 37
#define SD_D2  41              // pin 38
#define SD_D3  42              // pin 39
#define SD_CD  16              // pin 74  card detect (active low)

// -------------------------------------------------------------------------
// 100 Mbps Ethernet: ESP32-P4 EMAC in RMII mode driving an external IP101GA
// PHY.  In this design the PHY sources the 50 MHz RMII REF_CLK and feeds the
// P4's 50M_CLKI, so the EMAC runs in clock-input mode (EMAC_CLK_EXT_IN).
// -------------------------------------------------------------------------
#define USE_ETH
#define ETH_PHY_ADDR       1
#define ETH_MDC_GPIO       26  // pin 22
#define ETH_MDIO_GPIO      27  // pin 23  4.7k pull-up
#define ETH_CRS_DV_GPIO    28  // pin 24
#define ETH_RXD0_GPIO      29  // pin 25
#define ETH_RXD1_GPIO      30  // pin 26
#define ETH_RMII_CLK_GPIO  32  // pin 28  input from PHY (50M_CLKO)
#define ETH_RMII_CLK_INPUT     // 50 MHz REF_CLK is supplied by the PHY
#define ETH_TX_EN_GPIO     33  // pin 29
#define ETH_TXD0_GPIO      34  // pin 30
#define ETH_TXD1_GPIO      35  // pin 31  (strapping pin)

// 100 Mbps Ethernet is 100% dedicated to guest NE2000 network passthrough
// (pure L2 bridge, no ESP host IP stack). All VM management is performed
// natively via the built-in OSD control panel and file manager.


// -------------------------------------------------------------------------
// Audio output: I2S Philips standard -> PCM5102A DAC (no I2C, no MCLK needed,
// the PCM5102A recovers its clock from BCK/LRCK).
// -------------------------------------------------------------------------
#define MIXER_BUF_LEN 512
#define I2S_BCLK 20            // pin 78  -> PCM5102A BCK
#define I2S_WS   21            // pin 79  -> PCM5102A LRCK
#define I2S_DOUT 22            // pin 80  -> PCM5102A DIN
#define I2S_MCLK (-1)          // no master clock on this codec
#define I2S_NUM  0