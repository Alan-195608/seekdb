cmake_minimum_required(VERSION 3.16)
project(MiniVectorDB LANGUAGES CXX)

add_executable(minivectordb main.cpp)
target_compile_features(minivectordb PRIVATE cxx_std_20)
target_compile_options(minivectordb PRIVATE -Wall -Wextra -Wpedantic)
