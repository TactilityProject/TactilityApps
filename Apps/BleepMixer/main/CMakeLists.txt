include("$ENV{TACTILITY_SDK_PATH}/TactilitySDK.cmake")

file(GLOB_RECURSE SOURCE_FILES Source/*.c*)

# Bleep music (shared with Voidskim)
set(BLEEP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../Libraries/Bleep)
file(GLOB BLEEP_SOURCES ${BLEEP_DIR}/Source/*.cpp)

tactility_component_register(
    SRCS ${SOURCE_FILES} ${BLEEP_SOURCES}
    INCLUDE_DIRS "${BLEEP_DIR}/Include"
)

# ESP-IDF sets COMPONENT_LIB; host (linux/macOS) builds name the library after the project.
if (NOT DEFINED COMPONENT_LIB)
    set(COMPONENT_LIB ${PROJECT_NAME})
endif()

# Mach-O links shared libraries with undefined-symbol errors by default, but posix apps are
# dlopen()ed into a running Tactility process and resolve Tactility's own symbols at load time
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    target_link_options(${PROJECT_NAME} PRIVATE -Wl,-undefined,dynamic_lookup)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${PROJECT_NAME}>
                ${CMAKE_BINARY_DIR}/lib${PROJECT_NAME}.so
        COMMENT "Publishing lib${PROJECT_NAME}.so for the posix app loader"
    )
endif()
