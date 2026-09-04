cmake_minimum_required(VERSION 4.4)

project(game_3d LANGUAGES CXX)

find_package(
    engine 0.5.0 EXACT CONFIG REQUIRED
    COMPONENTS graphics
)

if(NOT TARGET engine::platform)
    message(FATAL_ERROR "engine::platform was not imported by find_package(engine).")
endif()
if(NOT TARGET engine::vulkan_graphics)
    message(FATAL_ERROR "engine::vulkan_graphics was not imported by find_package(engine).")
endif()

find_program(GAME_3D_GLSLC NAMES glslc REQUIRED)

set(GAME_3D_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/shaders")
set(GAME_3D_VERTEX_SPV "${GAME_3D_SHADER_DIR}/cube.vert.spv")
set(GAME_3D_FRAGMENT_SPV "${GAME_3D_SHADER_DIR}/cube.frag.spv")

add_custom_command(
    OUTPUT "${GAME_3D_VERTEX_SPV}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${GAME_3D_SHADER_DIR}"
    COMMAND "${GAME_3D_GLSLC}"
        --target-env=vulkan1.3
        -fshader-stage=vert
        -o "${GAME_3D_VERTEX_SPV}"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/cube.vert"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/cube.vert"
    VERBATIM
)

add_custom_command(
    OUTPUT "${GAME_3D_FRAGMENT_SPV}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${GAME_3D_SHADER_DIR}"
    COMMAND "${GAME_3D_GLSLC}"
        --target-env=vulkan1.3
        -fshader-stage=frag
        -o "${GAME_3D_FRAGMENT_SPV}"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/cube.frag"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/cube.frag"
    VERBATIM
)

add_custom_target(game_3d_shaders DEPENDS "${GAME_3D_VERTEX_SPV}" "${GAME_3D_FRAGMENT_SPV}")

add_executable(game_3d src/main.cpp)
add_dependencies(game_3d game_3d_shaders)
target_compile_definitions(
    game_3d
    PRIVATE
        GAME_3D_VERTEX_SPV="${GAME_3D_VERTEX_SPV}"
        GAME_3D_FRAGMENT_SPV="${GAME_3D_FRAGMENT_SPV}"
)
target_link_libraries(
    game_3d
    PRIVATE
        engine::platform
        engine::vulkan_graphics
)
