cmake_minimum_required(VERSION 3.20)
project(wasp_host)

set(CMAKE_CXX_STANDARD 20)

find_package(libzip REQUIRED)
find_package(PkgConfig REQUIRED)

pkg_check_modules(PORTAUDIO REQUIRED portaudio-2.0)
pkg_check_modules(RTMIDI    REQUIRED rtmidi)

add_executable(wasp_host main.cpp wasp_host.c)

target_include_directories(wasp_host PRIVATE
        /usr/local/include
        ${PORTAUDIO_INCLUDE_DIRS}
        ${RTMIDI_INCLUDE_DIRS}
        include
)

target_link_libraries(wasp_host PRIVATE
        libzip::zip
        /usr/local/lib/libwasmtime.a
        ${PORTAUDIO_LIBRARIES}
        ${RTMIDI_LIBRARIES}
        pthread
        dl
        m
)

target_compile_options(wasp_host PRIVATE -g -fsanitize=address)
target_link_options(wasp_host PRIVATE -fsanitize=address)