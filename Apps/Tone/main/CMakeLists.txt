include("$ENV{TACTILITY_SDK_PATH}/TactilitySDK.cmake")

file(GLOB_RECURSE SOURCE_FILES Source/*.c*)
tactility_component_register(SRCS ${SOURCE_FILES})

# Mach-O links shared libraries with undefined-symbol errors by default, but posix apps are
# dlopen()ed into a running Tactility process (app-posix-module) and resolve Tactility's own
# symbols at load time
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    target_link_options(${PROJECT_NAME} PRIVATE -Wl,-undefined,dynamic_lookup)
    # tactility.py packages a posix app by scanning for a "<name>.so" artifact (find_elf_file()).
    # On macOS CMake names a SHARED target ".dylib", so publish a ".so"-suffixed copy for it.
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${PROJECT_NAME}>
                ${CMAKE_BINARY_DIR}/lib${PROJECT_NAME}.so
        COMMENT "Publishing lib${PROJECT_NAME}.so for the posix app loader"
    )
endif()