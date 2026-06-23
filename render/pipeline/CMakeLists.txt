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
