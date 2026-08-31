include("$ENV{TACTILITY_SDK_PATH}/TactilitySDK.cmake")

file(GLOB_RECURSE SOURCE_FILES Source/*.c*)
file(GLOB_RECURSE UNIT_MODULE_FILES ../../../Libraries/M5UnitModules/Source/*.c*)
tactility_component_register(SRCS ${SOURCE_FILES} ${UNIT_MODULE_FILES} INCLUDE_DIRS ../../../Libraries/M5UnitModules/Include)
