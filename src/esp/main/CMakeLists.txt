idf_component_register(SRCS "esp_main.c"
"../../adlib.c"
"../../fmopl.c"
"../../fpu.c"
"../../i386.c"
"../../jit.c"
"../../i8042.c"
"../../i8254.c"
"../../i8259.c"
"../../ide.c"
"../../ini.c"
"../../pc.c"
"../../misc.c"
"../../pci.c"
"../../vga.c"
"../../cirrus_vga.c"
"../../cirrus_opencl.c"
"../../cirrus_ppa.c"
"../../vgafont.c"
"../../ne2000.c"
"../../i8257.c"
"../../sb16.c"
"../../pcspk.c"
"wifi.c"
"lcd_st7701.c"
"i2s.c"
"storage.c"
"usb_hid.c"
"usb_input.c"
INCLUDE_DIRS ""
PRIV_REQUIRES esp_driver_ppa esp_psram esp_mm esp_lcd esp_driver_i2s esp_driver_i2c driver fatfs spiffs sdmmc nvs_flash esp_timer esp_driver_ledc esp_driver_gpio esp_codec_dev esp_partition esp_event esp_wifi vfs heap esp_system esp_common esp_hw_support)

if(NOT DEFINED BOARD)
    set(BOARD "jc4880p443")
endif()

message(STATUS "Building for BOARD=${BOARD} (ESP32-P4 Dedicated)")

add_definitions(-include ${CMAKE_CURRENT_SOURCE_DIR}/board_${BOARD}.h)
add_definitions(-DI386_ENABLE_FPU)
add_definitions(-DUSE_FMOPL)

set_source_files_properties("../../adlib.c" PROPERTIES COMPILE_FLAGS "-Wno-all")
set_source_files_properties("../../fmopl.c" PROPERTIES COMPILE_FLAGS "-Wno-all -fsingle-precision-constant")
set_source_files_properties("../../fpu.c" PROPERTIES COMPILE_FLAGS "-Wno-all")
set_source_files_properties("../../i386.c" PROPERTIES COMPILE_FLAGS "-Wno-all -O3")
set_source_files_properties("../../jit.c" PROPERTIES COMPILE_FLAGS "-Wno-all -O3")
set_source_files_properties("../../i8042.c" PROPERTIES COMPILE_FLAGS "-Wno-all")
set_source_files_properties("../../vga.c" PROPERTIES COMPILE_FLAGS "-Wno-all -O3")
set_source_files_properties("../../cirrus_vga.c" PROPERTIES COMPILE_FLAGS "-Wno-all -O3")
set_source_files_properties("../../pc.c" PROPERTIES COMPILE_FLAGS "-Wno-all -O3")
set_source_files_properties("../../ne2000.c" PROPERTIES COMPILE_FLAGS "-Wno-all")
set_source_files_properties("../../sb16.c" PROPERTIES COMPILE_FLAGS "-Wno-all")
set_source_files_properties("../../i8257.c" PROPERTIES COMPILE_FLAGS "-Wno-all")

if(NOT EXISTS "../../fmopl.inc")
    message(FATAL_ERROR "Please run `make prepare` first")
endif()
