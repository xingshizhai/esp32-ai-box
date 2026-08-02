# Shared project bootstrap. The product is selected by entering ai-pet/ or
# anti-pet/; only the hardware board remains configurable.
if(NOT DEFINED APP_PROJECT_NAME)
    message(FATAL_ERROR "APP_PROJECT_NAME must be set by the product project")
endif()

get_filename_component(APP_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

if(NOT DEFINED APP_BOARD)
    if(DEFINED APP_DEFAULT_BOARD)
        set(APP_BOARD "${APP_DEFAULT_BOARD}")
    else()
        set(APP_BOARD "esp32s3_box3")
    endif()
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

message(STATUS "APP_PRODUCT = ${APP_PROJECT_NAME}")
message(STATUS "APP_BOARD = ${APP_BOARD}")

set(SDKCONFIG_DEFAULTS
    "${APP_REPO_ROOT}/sdkconfig.defaults.esp32s3"
    "${APP_REPO_ROOT}/sdkconfig.defaults.${APP_BOARD}"
    "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.defaults"
)

set(EXTRA_COMPONENT_DIRS
    "${APP_REPO_ROOT}/components"
    "${APP_REPO_ROOT}/components/boards/${APP_BOARD}"
)
