include("$ENV{TACTILITY_SDK_PATH}/TactilitySDK.cmake")

file(GLOB_RECURSE SOURCE_FILES Source/*.c)
tactility_component_register(SRCS ${SOURCE_FILES})

# neatvi code style (third-party, kept unmodified)
set(NEATVI_COMPILE_OPTIONS -Wno-missing-field-initializers -Wno-char-subscripts -Wno-implicit-fallthrough -Wno-format-truncation)
if (DEFINED COMPONENT_LIB)
    target_compile_options(${COMPONENT_LIB} PRIVATE ${NEATVI_COMPILE_OPTIONS})
else ()
    target_compile_options(${PROJECT_NAME} PRIVATE ${NEATVI_COMPILE_OPTIONS})
endif ()
