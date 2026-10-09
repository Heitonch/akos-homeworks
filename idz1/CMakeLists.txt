cmake_minimum_required(VERSION 3.16)
project(idz1 C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_executable(idz1 main.c simulation.c)
target_compile_options(idz1 PRIVATE -Wall -Wextra -Wpedantic)