cmake_minimum_required(VERSION 4.3)

project(lokiverse_smoke LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(lokiverse REQUIRED)

add_executable(smoke main.cpp)
target_link_libraries(smoke PRIVATE lokiverse::lokiverse)
target_link_options(smoke PRIVATE -static)
