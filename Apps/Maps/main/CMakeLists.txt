include("$ENV{TACTILITY_SDK_PATH}/TactilitySDK.cmake")

file(GLOB_RECURSE SOURCE_FILES Source/*.c*)
tactility_component_register(
    SRCS ${SOURCE_FILES}
    REQUIRES graphics-module binfont-module location-module http-module esp_http_client esp_rom mbedtls
)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -Wno-format-truncation)
# The search index must match the PC's byte for byte (M6): no fused multiply-add in its float maths (MSVC on the PC
# does not fuse; GCC would, and one house at the 50 m limit then went to another street)
file(GLOB SEARCH_SOURCES Source/search/*.cpp)
set_source_files_properties(${SEARCH_SOURCES} PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
