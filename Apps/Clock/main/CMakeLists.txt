include("$ENV{TACTILITY_SDK_PATH}/TactilitySDK.cmake")

file(GLOB_RECURSE SOURCE_FILES Source/*.c*)
tactility_component_register(SRCS ${SOURCE_FILES})

if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    target_link_options(${PROJECT_NAME} PRIVATE -Wl,-undefined,dynamic_lookup)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${PROJECT_NAME}>
                ${CMAKE_BINARY_DIR}/lib${PROJECT_NAME}.so
        COMMENT "Publishing lib${PROJECT_NAME}.so for the posix app loader"
    )
endif()
