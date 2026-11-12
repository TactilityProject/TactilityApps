include("$ENV{TACTILITY_SDK_PATH}/TactilitySDK.cmake")

file(GLOB_RECURSE SOURCE_FILES Source/*.c*)
tactility_component_register(
    SRCS ${SOURCE_FILES}
    REQUIRES graphics-module binfont-module location-module http-module esp_http_client esp_rom mbedtls
)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -Wno-format-truncation)
