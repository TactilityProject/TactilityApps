include("$ENV{TACTILITY_SDK_PATH}/TactilitySDK.cmake")

file(GLOB_RECURSE SOURCE_FILES Source/*.c*)
file(GLOB_RECURSE GFX_FILES ../../../Libraries/Gfx/Source/*.c*)
file(GLOB_RECURSE INPUT_FILES ../../../Libraries/Input/Source/*.c*)

tactility_component_register(
    SRCS ${SOURCE_FILES} ${GFX_FILES} ${INPUT_FILES}
    INCLUDE_DIRS "Include" ../../../Libraries/Gfx/Include ../../../Libraries/Input/Include
    REQUIRES graphics-module esp_driver_ppa
)
