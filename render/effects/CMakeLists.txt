cmake_minimum_required(VERSION 3.18)
project(RenderEffects CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_definitions(NOMINMAX)

# Download and extract DirectML NuGet package if not present
set(DIRECTML_VERSION "1.15.2")
set(DIRECTML_DIR "${CMAKE_CURRENT_BINARY_DIR}/directml")

if(NOT EXISTS "${DIRECTML_DIR}/bin/DirectML.dll")
    message(STATUS "Downloading DirectML NuGet package version ${DIRECTML_VERSION}...")
    file(DOWNLOAD 
        "https://www.nuget.org/api/v2/package/Microsoft.AI.DirectML/${DIRECTML_VERSION}" 
        "${CMAKE_CURRENT_BINARY_DIR}/directml.zip"
        SHOW_PROGRESS
    )
    message(STATUS "Extracting DirectML NuGet package...")
    file(ARCHIVE_EXTRACT 
        INPUT "${CMAKE_CURRENT_BINARY_DIR}/directml.zip"
        DESTINATION "${DIRECTML_DIR}"
    )
endif()

# Set up include and lib directories
set(DIRECTML_INCLUDE_DIR "${DIRECTML_DIR}/include")
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    if(CMAKE_SYSTEM_PROCESSOR STREQUAL "ARM64" OR CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64" OR CMAKE_SYSTEM_PROCESSOR STREQUAL "ARM64EC")
        set(DIRECTML_LIB "${DIRECTML_DIR}/bin/arm64-win/DirectML.lib")
        set(DIRECTML_DLL "${DIRECTML_DIR}/bin/arm64-win/DirectML.dll")
    else()
        set(DIRECTML_LIB "${DIRECTML_DIR}/bin/x64-win/DirectML.lib")
        set(DIRECTML_DLL "${DIRECTML_DIR}/bin/x64-win/DirectML.dll")
    endif()
else()
    set(DIRECTML_LIB "${DIRECTML_DIR}/bin/x86-win/DirectML.lib")
    set(DIRECTML_DLL "${DIRECTML_DIR}/bin/x86-win/DirectML.dll")
endif()

add_executable(effects_benchmark 
    src/main.cpp
    src/directml_context.cpp
    src/color_grade_effect.cpp
    src/texture_pool.cpp
    src/effects_pipeline.cpp
)

target_include_directories(effects_benchmark PRIVATE 
    src 
    ${DIRECTML_INCLUDE_DIR}
)

# We link DXGI and D3D12 using standard system libraries
target_link_libraries(effects_benchmark PRIVATE 
    d3d12
    dxgi
    ${DIRECTML_LIB}
)

# Copy DirectML.dll to the output directory post-build
add_custom_command(TARGET effects_benchmark POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${DIRECTML_DLL}"
    $<TARGET_FILE_DIR:effects_benchmark>
)
