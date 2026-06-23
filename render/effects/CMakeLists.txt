cmake_minimum_required(VERSION 3.18)
project(RenderEffects CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_definitions(NOMINMAX)

if(NOT EXISTS "${CMAKE_CURRENT_BINARY_DIR}/directml")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/build/directml")
        message(STATUS "Copying pre-extracted DirectML from source build directory...")
        file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/build/directml" DESTINATION "${CMAKE_CURRENT_BINARY_DIR}")
    endif()
endif()


add_executable(effects_benchmark 
    src/benchmark.cpp
    src/directml_context.cpp
    src/color_grade_effect.cpp
    src/texture_pool.cpp
    src/effects_pipeline.cpp
    src/color_grade_compute.cpp
)

target_include_directories(effects_benchmark PRIVATE 
    src 
    ${CMAKE_CURRENT_BINARY_DIR}/directml/include
)

# We link DXGI and D3D12 using standard system libraries
target_link_libraries(effects_benchmark PRIVATE 
    d3d12
    dxgi
    d3dcompiler
    ${CMAKE_CURRENT_BINARY_DIR}/directml/bin/arm64-win/DirectML.lib
)

# Copy color_grade.hlsl to the output directory post-build
add_custom_command(TARGET effects_benchmark POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/color_grade.hlsl"
    $<TARGET_FILE_DIR:effects_benchmark>
)
