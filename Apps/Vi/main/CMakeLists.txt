include("$ENV{TACTILITY_SDK_PATH}/TactilitySDK.cmake")

file(GLOB_RECURSE SOURCE_FILES Source/*.c)
tactility_component_register(SRCS ${SOURCE_FILES} INCLUDE_DIRS "Include")

# BusyBox code style. ESP-IDF builds a component library, POSIX builds the project target directly.
if (DEFINED COMPONENT_LIB)
    target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-char-subscripts -Wno-implicit-fallthrough)
else ()
    target_compile_options(${PROJECT_NAME} PRIVATE -Wno-char-subscripts -Wno-implicit-fallthrough)
endif ()
