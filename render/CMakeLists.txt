cmake_minimum_required(VERSION 3.18)
project(Render CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Add effects and pipeline subdirectories
add_subdirectory(effects)
add_subdirectory(pipeline)
