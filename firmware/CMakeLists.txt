# Top-level CMake file for the Pilot Kit Box firmware.
# Target: ESP32-P4 (set in sdkconfig.defaults via CONFIG_IDF_TARGET).
cmake_minimum_required(VERSION 3.16)

# Pull components from the in-tree `components/` directory (esp32-rtl-sdr lives there).
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(pilot_kit_box)
