cmake_minimum_required(VERSION 3.16)
project(MiniVectorDB LANGUAGES CXX)

find_package(PkgConfig REQUIRED)
pkg_check_modules(JANSSON REQUIRED IMPORTED_TARGET jansson>=2.14)

add_executable(minivectordb main.cpp)
target_compile_features(minivectordb PRIVATE cxx_std_20)
target_compile_options(minivectordb PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(minivectordb PRIVATE PkgConfig::JANSSON)
