# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Board selection: P4 Only target default jc4880p443.
if(NOT DEFINED BOARD)
    if(DEFINED ENV{BOARD})
        set(BOARD $ENV{BOARD})
    else()
        set(BOARD "jc4880p443")
    endif()
endif()
# Always sync sdkconfig from the board-specific file.
set(BOARD_SDKCONFIG "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.${BOARD}")
if(EXISTS "${BOARD_SDKCONFIG}")
    file(COPY_FILE "${BOARD_SDKCONFIG}" "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig" ONLY_IF_DIFFERENT)
endif()
set(SDKCONFIG_DEFAULTS "${BOARD_SDKCONFIG}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tiny386)
