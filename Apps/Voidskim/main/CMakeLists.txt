include("$ENV{TACTILITY_SDK_PATH}/TactilitySDK.cmake")

# App sources
file(GLOB_RECURSE APP_SOURCES Source/*.c*)

# Bleep music (shared with BleepMixer)
set(BLEEP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../Libraries/Bleep)
file(GLOB BLEEP_SOURCES ${BLEEP_DIR}/Source/*.cpp)

# Jet 3D renderer (git submodule, https://github.com/cubecoders/jet), minus its examples.
# Configured by Source/JetConfig.hpp.
set(JET_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../Libraries/Jet/src)
file(GLOB JET_SOURCES ${JET_DIR}/*.cpp)
list(FILTER JET_SOURCES EXCLUDE REGEX "(Example|Sample|Scene)\\.cpp$")
# Scene.cpp is compiled through a wrapper that avoids its static constructor (see the wrapper)
list(APPEND JET_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/JetWrappers/JetScene.cpp)

tactility_component_register(
    SRCS ${APP_SOURCES} ${JET_SOURCES} ${BLEEP_SOURCES}
    INCLUDE_DIRS "Source" "${JET_DIR}" "${BLEEP_DIR}/Include"
    REQUIRES TactilitySDK driver esp_driver_ppa esp_mm
)

# ESP-IDF sets COMPONENT_LIB; host (linux/macOS) builds name the library after the project.
if (NOT DEFINED COMPONENT_LIB)
    set(COMPONENT_LIB ${PROJECT_NAME})
endif()

if (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    target_link_options(${PROJECT_NAME} PRIVATE -Wl,-undefined,dynamic_lookup)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${PROJECT_NAME}>
                ${CMAKE_BINARY_DIR}/lib${PROJECT_NAME}.so
        COMMENT "Publishing lib${PROJECT_NAME}.so for the posix app loader"
    )
endif()

# Jet is third-party: don't fail the build on its warnings
# JetPlatform.h turns its internal-RAM placement attributes into no-ops (see that file).
set_source_files_properties(${JET_SOURCES} PROPERTIES COMPILE_FLAGS
    "-Wno-error -w -include ${CMAKE_CURRENT_SOURCE_DIR}/Source/JetPlatform.h")

# Software 3D is CPU bound: build with full optimisation (the firmware's sdkconfig uses -Og)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
