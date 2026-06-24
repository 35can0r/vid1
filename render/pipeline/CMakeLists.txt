cmake_minimum_required(VERSION 3.18)
project(RenderPipeline CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(VCPKG_DIR "D:/k50i/vcpkg")
set(VCPKG_INSTALLED_DIR "${VCPKG_DIR}/installed/arm64-windows-static-md")

# Create the decoder static library
add_library(decoder STATIC
    src/video_decoder.cpp
)

target_include_directories(decoder PUBLIC
    src
    "${VCPKG_INSTALLED_DIR}/include"
)

# Link FFmpeg and Windows libraries
target_link_libraries(decoder PUBLIC
    d3d11
    d3d12
    dxgi
    d3dcompiler
    "${VCPKG_INSTALLED_DIR}/lib/avcodec.lib"
    "${VCPKG_INSTALLED_DIR}/lib/avformat.lib"
    "${VCPKG_INSTALLED_DIR}/lib/avutil.lib"
    "${VCPKG_INSTALLED_DIR}/lib/swscale.lib"
    bcrypt
    ws2_32
    secur32
    user32
    ncrypt
    crypt32
    mfplat
    mfuuid
    strmiids
    ole32
)

# Test executable for the video decoder
add_executable(decoder_test
    src/decoder_test.cpp
    ../effects/src/texture_pool.cpp
)

target_include_directories(decoder_test PRIVATE
    src
    ../effects/src
)

target_link_libraries(decoder_test PRIVATE
    decoder
)

# Create the pipeline static library
add_library(pipeline STATIC
    src/compositor.cpp
    src/preview_presenter.cpp
    src/render_engine.cpp
    ../effects/src/texture_pool.cpp
)

target_include_directories(pipeline PUBLIC
    src
    include
    ../effects/src
)

target_link_libraries(pipeline PUBLIC
    d3d12
    dxgi
    d3dcompiler
)

# HLSL Compilation for Compositor
add_custom_command(
    OUTPUT compositor_vs.cso
    COMMAND dxc -T vs_6_0 -E VSMain -Fo compositor_vs.cso ${CMAKE_CURRENT_SOURCE_DIR}/shaders/compositor.hlsl
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/compositor.hlsl
    COMMENT "Compiling compositor VS"
)

add_custom_command(
    OUTPUT compositor_ps.cso
    COMMAND dxc -T ps_6_0 -E PSMain -Fo compositor_ps.cso ${CMAKE_CURRENT_SOURCE_DIR}/shaders/compositor.hlsl
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/compositor.hlsl
    COMMENT "Compiling compositor PS"
)

# Test executable for the compositor
add_executable(compositor_test
    src/compositor_test.cpp
    ../effects/src/texture_pool.cpp
    compositor_vs.cso
    compositor_ps.cso
)

target_include_directories(compositor_test PRIVATE
    src
    include
    ../effects/src
)

target_link_libraries(compositor_test PRIVATE
    pipeline
    decoder
)

add_custom_command(TARGET compositor_test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_BINARY_DIR}/compositor_vs.cso"
        "$<TARGET_FILE_DIR:compositor_test>/compositor_vs.cso"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_BINARY_DIR}/compositor_ps.cso"
        "$<TARGET_FILE_DIR:compositor_test>/compositor_ps.cso"
    COMMENT "Copying compiled shaders to output directory"
)
