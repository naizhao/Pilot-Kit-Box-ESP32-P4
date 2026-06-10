# Top-level CMake file for the Pilot Kit Box firmware.
# Target: ESP32-P4 (set in sdkconfig.defaults via CONFIG_IDF_TARGET).
cmake_minimum_required(VERSION 3.16)

# Pull components from the in-tree `components/` directory (esp32-rtl-sdr lives there).
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")

# Local version = version.txt + current git short hash, e.g. "v0.8.0-ce49d37".
# An explicit -DPROJECT_VER (used by release CI) wins and stays exactly equal
# to the release tag so firmware metadata and asset names remain aligned.
if(NOT DEFINED PROJECT_VER OR PROJECT_VER STREQUAL "")
    file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/version.txt" PK_BASE_VER LIMIT_COUNT 1)
    execute_process(COMMAND git rev-parse --short HEAD
                    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
                    OUTPUT_VARIABLE PK_GIT_SHA OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
    if(PK_GIT_SHA)
        set(PROJECT_VER "${PK_BASE_VER}-${PK_GIT_SHA}")
    else()
        set(PROJECT_VER "${PK_BASE_VER}")
    endif()
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(pilot_kit_box)
