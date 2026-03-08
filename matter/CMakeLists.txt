cmake_minimum_required(VERSION 3.16)

# ESP-Matter SDK einbinden (muss vor project() stehen)
# Pfad: export ESP_MATTER_PATH=/path/to/esp-matter
if(DEFINED ENV{ESP_MATTER_PATH})
    include($ENV{ESP_MATTER_PATH}/connectedhomeip/connectedhomeip/src/app/idf_component_manager/CMakeLists.txt OPTIONAL)
    set(EXTRA_COMPONENT_DIRS
        $ENV{ESP_MATTER_PATH}/components
        $ENV{ESP_MATTER_PATH}/examples/common
    )
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32c6_matter_vital_gateway)
