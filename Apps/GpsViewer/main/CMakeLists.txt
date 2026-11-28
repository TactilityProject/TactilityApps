include("$ENV{TACTILITY_SDK_PATH}/TactilitySDK.cmake")

file(GLOB_RECURSE SOURCE_FILES Source/*.c*)
tactility_component_register(
    SRCS ${SOURCE_FILES}
    INCLUDE_DIRS $ENV{TACTILITY_SDK_PATH}/Libraries/minmea/include $ENV{TACTILITY_SDK_PATH}/Modules/gps-module/include $ENV{TACTILITY_SDK_PATH}/Modules/location-module/include $ENV{TACTILITY_SDK_PATH}/Libraries/TactilityFreeRtos/Include
    REQUIRES gps-module location-module
)
