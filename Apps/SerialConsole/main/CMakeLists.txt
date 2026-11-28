include("$ENV{TACTILITY_SDK_PATH}/TactilitySDK.cmake")

file(GLOB_RECURSE SOURCE_FILES Source/*.c*)
tactility_component_register(SRCS ${SOURCE_FILES} INCLUDE_DIRS $ENV{TACTILITY_SDK_PATH}/Libraries/TactilityFreeRtos/Include)