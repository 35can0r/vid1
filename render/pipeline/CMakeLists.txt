cmake_minimum_required(VERSION 3.10)
project(RenderPipeline)

set(CMAKE_CXX_STANDARD 17)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED libavcodec libavformat libswscale libavutil)

add_library(decoder src/decoder.cpp)
target_include_directories(decoder PUBLIC src)
target_include_directories(decoder PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_libraries(decoder PRIVATE ${FFMPEG_LIBRARIES})
