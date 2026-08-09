file(GLOB_RECURSE SOURCE_FILES
    Source/*.c*
)

idf_component_register(
    SRCS ${SOURCE_FILES}
    REQUIRES TactilitySDK bootloader_support esp_app_format
)
