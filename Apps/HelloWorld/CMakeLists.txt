cmake_minimum_required(VERSION 3.20)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

if (DEFINED ENV{TACTILITY_SDK_PATH})
    set(TACTILITY_SDK_PATH $ENV{TACTILITY_SDK_PATH})
else()
    set(TACTILITY_SDK_PATH "../../release/TactilitySDK")
    message(WARNING "⚠️ TACTILITY_SDK_PATH environment variable is not set, defaulting to ${TACTILITY_SDK_PATH}")
endif()

include("${TACTILITY_SDK_PATH}/TactilitySDK.cmake")

# Must be set before project() - ESP-IDF resolves components at that point, so setting these
# from inside the tactility_project() macro (which necessarily runs after project(), since it
# also calls project_elf()) would be too late.
set(EXTRA_COMPONENT_DIRS
    ${TACTILITY_SDK_PATH}
    "${TACTILITY_SDK_PATH}/Libraries/TactilityFreeRtos"
    "${TACTILITY_SDK_PATH}/Modules"
)

project(HelloWorld)
tactility_project(HelloWorld)
