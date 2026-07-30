cmake_minimum_required(VERSION 3.5)

set(EXTRA_COMPONENT_DIRS "main/components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32-ai-box)

# "storage" SPIFFS partition (see partitions.csv) carries the full-coverage
# CJK font binary; it is loaded at runtime via LVGL's binfont loader since
# it is too large to link into the 2MB app partition. FLASH_IN_PROJECT makes
# `idf.py flash` write it alongside the app image automatically.
spiffs_create_partition_image(storage main/assets/spiffs_storage FLASH_IN_PROJECT)
