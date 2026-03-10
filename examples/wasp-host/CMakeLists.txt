cmake_minimum_required(VERSION 3.20)
project(wasp_host)

set(CMAKE_CXX_STANDARD 20)

add_executable(wasp_host main.cpp wasp_host.c)

target_include_directories(wasp_host PRIVATE /usr/local/include include)

find_package(libzip REQUIRED)

target_link_libraries(wasp_host PRIVATE libzip::zip
    libwasmtime.a
    pthread
    dl
    m
)

target_compile_options(wasp_host PRIVATE -g -fsanitize=address)
target_link_options(wasp_host PRIVATE -fsanitize=address)