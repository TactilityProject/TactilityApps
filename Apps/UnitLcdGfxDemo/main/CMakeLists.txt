include("$ENV{TACTILITY_SDK_PATH}/TactilitySDK.cmake")

file(GLOB_RECURSE SOURCE_FILES Source/*.c*)
file(GLOB_RECURSE GFX_FILES ../../../Libraries/Gfx/Source/*.c*)
file(GLOB_RECURSE INPUT_FILES ../../../Libraries/Input/Source/*.c*)
file(GLOB UNIT_FILES
    ../../../Libraries/M5UnitModules/Source/UnitLcd.cpp
    ../../../Libraries/M5UnitModules/Source/UnitPaHub.cpp
)

tactility_component_register(
    SRCS ${SOURCE_FILES} ${GFX_FILES} ${INPUT_FILES} ${UNIT_FILES}
    INCLUDE_DIRS
        "Include"
        ../../../Libraries/Gfx/Include
        ../../../Libraries/Input/Include
        ../../../Libraries/M5UnitModules/Include
        ../../../Libraries/UnitLcdGfx/Include
    REQUIRES graphics-module esp_driver_ppa
)
