# tactility-cmakelists-version: 1
cmake_minimum_required(VERSION 3.20)

if (NOT DEFINED ENV{TACTILITY_SDK_PATH})
    message(FATAL_ERROR "TACTILITY_SDK_PATH environment variable is not set")
endif()

get_filename_component(TACTILITY_SDK_PATH "$ENV{TACTILITY_SDK_PATH}" ABSOLUTE)
include("${TACTILITY_SDK_PATH}/TactilitySDK.cmake")

tactility_project_pre(tactility.mystifyspidemo.ili9341-panel-settings)
project(tactility.mystifyspidemo.ili9341-panel-settings)
tactility_project_post(tactility.mystifyspidemo.ili9341-panel-settings)
