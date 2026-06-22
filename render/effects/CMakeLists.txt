cmake_minimum_required(VERSION 3.18)
project(RenderEffects CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_definitions(NOMINMAX)

add_executable(effects_benchmark 
    src/main.cpp
    src/directml_context.cpp
    src/color_grade_effect.cpp
    src/texture_pool.cpp
    src/effects_pipeline.cpp
    src/color_grade_compute.cpp
)

target_include_directories(effects_benchmark PRIVATE 
    src 
)

# We link DXGI and D3D12 using standard system libraries
target_link_libraries(effects_benchmark PRIVATE 
    d3d12
    dxgi
    d3dcompiler
)

# Copy color_grade.hlsl to the output directory post-build
add_custom_command(TARGET effects_benchmark POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/color_grade.hlsl"
    $<TARGET_FILE_DIR:effects_benchmark>
)
