cmake_minimum_required(VERSION 3.5)

# ── Board selection ────────────────────────────────────────────────────────
#
# Priority (highest → lowest):
#   1. -DAPP_BOARD=<name> on the cmake/idf.py command line
#   2. CONFIG_APP_BOARD_* parsed from sdkconfig (updated immediately by menuconfig)
#   3. Default: esp32s3_box3
#
# sdkconfig is read directly (not sdkconfig.cmake) because sdkconfig.cmake is
# only generated during the build phase — reading it here would be one build
# cycle behind after a menuconfig change, causing the wrong board component
# to link.
if(NOT DEFINED APP_BOARD)
    set(APP_BOARD "esp32s3_box3")   # default

    set(_sdkconfig "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig")
    if(EXISTS "${_sdkconfig}")
        file(STRINGS "${_sdkconfig}" _sdk_lines REGEX "^CONFIG_APP_BOARD_")
        foreach(_line IN LISTS _sdk_lines)
            if(_line MATCHES "^CONFIG_APP_BOARD_ESP32S3_LCD_EV_BOARD=y")
                set(APP_BOARD "esp32s3_lcd_ev_board")
            elseif(_line MATCHES "^CONFIG_APP_BOARD_CUSTOM=y")
                set(APP_BOARD "custom")
            endif()
        endforeach()
    endif()
endif()

message(STATUS "APP_BOARD = ${APP_BOARD}")

# ── sdkconfig defaults (stacked, last file wins; each only added if present) ─
set(SDKCONFIG_DEFAULTS "")
foreach(_defaults "sdkconfig.defaults" "sdkconfig.defaults.esp32s3" "sdkconfig.defaults.${APP_BOARD}")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_defaults}")
        list(APPEND SDKCONFIG_DEFAULTS "${_defaults}")
    endif()
endforeach()

# ── Component paths ──────────────────────────────────────────────────────────
set(EXTRA_COMPONENT_DIRS
    "main/components"
    "main/components/boards/${APP_BOARD}"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32-ai-box)

# "storage" SPIFFS partition (see partitions.csv) carries the full-coverage
# CJK font binary; it is loaded at runtime via LVGL's binfont loader since
# it is too large to link into the 2MB app partition. FLASH_IN_PROJECT makes
# `idf.py flash` write it alongside the app image automatically.
spiffs_create_partition_image(storage main/assets/spiffs_storage FLASH_IN_PROJECT)
