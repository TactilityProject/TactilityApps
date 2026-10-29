cmake_minimum_required(VERSION 3.16)
project(TactilityMapsHost CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(APP_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/../main/Source)

add_library(miniz STATIC third_party/miniz/miniz.c)
target_include_directories(miniz PUBLIC third_party/miniz)

file(GLOB CORE_SOURCES
    ${APP_SOURCE}/tiles/*.cpp
    ${APP_SOURCE}/mvt/*.cpp
    ${APP_SOURCE}/render/*.cpp
    ${APP_SOURCE}/ui/*.cpp
)
add_library(mapcore STATIC ${CORE_SOURCES} PlatformHost.cpp)
target_link_libraries(mapcore PUBLIC miniz)
if (MSVC)
    target_compile_options(mapcore PRIVATE /W4 /wd4100)
    target_compile_definitions(mapcore PUBLIC _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(mapcore PRIVATE -Wall -Wextra)
endif()

add_executable(render_tile render_tile.cpp Png.cpp)
target_link_libraries(render_tile mapcore)

if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests.cpp)
    add_executable(tests tests.cpp Png.cpp)
    target_link_libraries(tests mapcore)
    target_compile_definitions(tests PRIVATE FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../tests/fixtures")
endif()

add_executable(prepare_tiles prepare_tiles.cpp)
target_link_libraries(prepare_tiles mapcore)

add_executable(ui_preview ui_preview.cpp Png.cpp)
target_link_libraries(ui_preview mapcore)
target_compile_definitions(ui_preview PRIVATE FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../tests/fixtures")
