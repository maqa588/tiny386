if(NOT DEFINED BOARD)
    set(BOARD "wt0132p4_a1")
endif()

message(STATUS "Building for BOARD=${BOARD} (target ${IDF_TARGET})")

# Board-dependent component requirements.  The ESP32-P4 board has no WiFi radio
# (Ethernet only), so esp_wifi is only pulled in for targets that support it
# natively (the ESP32-S31 board, which sets USE_WIFI).
set(MAIN_PRIV_REQUIRES esp_driver_ppa esp_psram esp_mm esp_lcd esp_driver_i2s esp_driver_i2c esp_driver_uart driver fatfs spiffs sdmmc nvs_flash esp_timer esp_driver_ledc esp_driver_gpio esp_partition esp_event esp_eth esp_netif vfs heap esp_system esp_common esp_hw_support)
if(NOT IDF_TARGET STREQUAL "esp32p4")
    list(APPEND MAIN_PRIV_REQUIRES esp_wifi)
endif()
# The LP (40 MHz) standby supervisor needs the LP core API on the HP side (see pm.c).
list(APPEND MAIN_PRIV_REQUIRES ulp)

# ...and its firmware, built as a separate ULP application by ulp_embed_binary()
# below, needs more than that.  ulp_embed_binary() passes this component's
# INTERFACE_INCLUDE_DIRECTORIES to the subproject as COMPONENT_INCLUDES (see
# components/ulp/project_include.cmake), and that property only carries *public*
# requirements.  The LP firmware's fixed source set pulls in HAL headers no
# board-level code ever includes (esp_hal_rtc_timer, esp_hal_i2s, esp_hal_gpspi,
# esp_hal_i2c, esp_hal_clock, esp_driver_gpio, esp_adc, ...), so they are added
# to the property explicitly below.  These requirements match the ones declared
# by the `ulp` component itself.  Generator expressions are used so that the
# final set of directories is resolved at generate time and does not depend on
# the order in which components get registered.
set(MAIN_LP_INCLUDE_COMPONENTS "")
if(CONFIG_ULP_COPROC_ENABLED AND CONFIG_ULP_COPROC_TYPE_LP_CORE)
    set(MAIN_LP_INCLUDE_COMPONENTS
        ulp esp_adc esp_driver_gpio esp_driver_uart esp_driver_i2s
        esp_hal_i2c esp_hal_i2s esp_hal_touch_sens esp_hal_gpspi
        esp_hal_pmu esp_hal_rtc_timer esp_hal_clock)
endif()

idf_component_register(SRCS "esp_main.c"
"../../adlib.c"
"../../fmopl.c"
"../../fpu.c"
"../../i386.c"
"../../jit_rv32_esp.c"
"../../i8042.c"
"../../i8254.c"
"../../i8259.c"
"../../ide.c"
"../../isofs.c"
"../../osd/osd.c"
"../../osd/microui.c"
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
"eth.c"
"lcd_st7701.c"
"lcd_rgb.c"
"i2s.c"
"storage.c"
"usb_hid.c"
"usb_input.c"
"pm.c"
INCLUDE_DIRS ""
PRIV_REQUIRES ${MAIN_PRIV_REQUIRES})

# Publish the include directories the LP standby firmware needs (see above).
# They must be spelled out as literal paths here: ulp_embed_binary() forwards this
# component's INTERFACE_INCLUDE_DIRECTORIES to the LP subproject as literal
# COMPONENT_INCLUDES values, and a nested $<TARGET_PROPERTY:...> generator
# expression is not expanded on that ExternalProject path (the component's
# placeholder target carries no include directories anyway).
foreach(_lp_comp ${MAIN_LP_INCLUDE_COMPONENTS})
    __component_get_target(_lp_tgt ${_lp_comp})
    if(_lp_tgt)
        idf_component_get_property(_lp_dir ${_lp_comp} COMPONENT_DIR)
    else()
        set(_lp_dir "${IDF_PATH}/components/${_lp_comp}")
    endif()
    foreach(_lp_sub "include" "${IDF_TARGET}/include")
        if(EXISTS "${_lp_dir}/${_lp_sub}")
            target_include_directories(${COMPONENT_LIB} INTERFACE "${_lp_dir}/${_lp_sub}")
        endif()
    endforeach()
endforeach()

add_definitions(-include ${CMAKE_CURRENT_SOURCE_DIR}/board_${BOARD}.h)
add_definitions(-DI386_ENABLE_FPU)
add_definitions(-DUSE_FMOPL)

set_source_files_properties("../../adlib.c" PROPERTIES COMPILE_FLAGS "-w")
set_source_files_properties("../../fmopl.c" PROPERTIES COMPILE_FLAGS "-w -fsingle-precision-constant")
set_source_files_properties("../../fpu.c" PROPERTIES COMPILE_FLAGS "-w")
set_source_files_properties("../../i386.c" PROPERTIES COMPILE_FLAGS "-w -O3")
set_source_files_properties("../../jit_rv32_esp.c" PROPERTIES COMPILE_FLAGS "-w -O3")
set_source_files_properties("../../i8042.c" PROPERTIES COMPILE_FLAGS "-w")
set_source_files_properties("../../vga.c" PROPERTIES COMPILE_FLAGS "-w -O3")
set_source_files_properties("../../cirrus_vga.c" PROPERTIES COMPILE_FLAGS "-w -O3")
set_source_files_properties("../../pc.c" PROPERTIES COMPILE_FLAGS "-w -O3")
set_source_files_properties("../../osd/osd.c" PROPERTIES COMPILE_FLAGS "-w")
set_source_files_properties("../../osd/microui.c" PROPERTIES COMPILE_FLAGS "-w")
set_source_files_properties("../../ne2000.c" PROPERTIES COMPILE_FLAGS "-w")
set_source_files_properties("../../sb16.c" PROPERTIES COMPILE_FLAGS "-w")
set_source_files_properties("../../i8257.c" PROPERTIES COMPILE_FLAGS "-w")

if(NOT EXISTS "../../fmopl.inc")
    message(FATAL_ERROR "Please run `make prepare` first")
endif()

# LP (40 MHz) core standby supervisor.  Only built when the board's sdkconfig
# enables the LP core; ulp_embed_binary compiles lp_core/lp_core_main.c with the
# LP RISC-V toolchain, embeds the binary into the app and generates lp_pm.h /
# lp_pm.ld with the `ulp_`-prefixed shared variables used by pm.c.
if(CONFIG_ULP_COPROC_ENABLED AND CONFIG_ULP_COPROC_TYPE_LP_CORE)
    ulp_embed_binary(lp_pm "lp_core/lp_core_main.c" "pm.c")
endif()
