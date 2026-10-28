include("$ENV{TACTILITY_SDK_PATH}/TactilitySDK.cmake")

file(GLOB_RECURSE SOURCE_FILES Source/*.c*)

tactility_component_register(
    SRCS ${SOURCE_FILES}
    INCLUDE_DIRS
        ../../common
    # Only for the ESP-IDF headers: at load time the module resolves the SPI/GPIO entry points
    # against the firmware's export table.
    REQUIRES esp_driver_spi esp_driver_gpio log
)
