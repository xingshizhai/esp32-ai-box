cmake_minimum_required(VERSION 3.16)
set(APP_PROJECT_NAME "ai-pet")
set(APP_DEFAULT_BOARD "esp32s3_box3")
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/esp_idf_project.cmake")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ai-pet)

spiffs_create_partition_image(storage "${APP_REPO_ROOT}/assets/spiffs_storage" FLASH_IN_PROJECT)
