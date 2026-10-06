include("$ENV{TACTILITY_SDK_PATH}/TactilitySDK.cmake")

file(GLOB_RECURSE SOURCE_FILES Source/*.c)
tactility_component_register(SRCS ${SOURCE_FILES})

# rcc700.c is kept unmodified: tactility.c provides main() and open() in its place.
# It declares its own libc prototypes (with int sizes), which don't match the builtin ones.
if (DEFINED COMPONENT_LIB)
    set(RCC700_TARGET ${COMPONENT_LIB})
else ()
    set(RCC700_TARGET ${PROJECT_NAME})
endif ()
set_source_files_properties(Source/rcc700.c
    TARGET_DIRECTORY ${RCC700_TARGET}
    PROPERTIES
        COMPILE_DEFINITIONS "open=rcc700_open"
        COMPILE_OPTIONS "-Umain;-Dmain=rcc700_main;-fno-builtin;-Wno-builtin-declaration-mismatch;-Wno-misleading-indentation"
)
