# The launcher: one NRO that lists what is on the card and hands over to it.
#
# It shares nothing with a game build - no Dawn, no Aurora, no translated code -
# because all it does is read a few directories and name the next NRO for the
# loader. That is why it is its own project rather than a target inside one: it
# builds in seconds, against libnx alone, whichever game happens to be
# configured.
#
#   cmake -S src/launcher -B build/launcher -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake
#   cmake --build build/launcher
cmake_minimum_required(VERSION 3.20)
project(wii-nx-launcher LANGUAGES CXX VERSION 2.0.0)

if (NOT CMAKE_SYSTEM_NAME STREQUAL "NintendoSwitch")
    message(FATAL_ERROR
        "The launcher is a Switch program: configure with\n"
        "  -DCMAKE_TOOLCHAIN_FILE=$ENV{DEVKITPRO}/cmake/Switch.cmake")
endif ()

add_executable(wii-nx-launcher main.cpp)
target_compile_features(wii-nx-launcher PRIVATE cxx_std_20)

nx_generate_nacp(wii-nx-launcher.nacp
    NAME "wii-nx" AUTHOR "nx-mod" VERSION "1.0.0")
nx_create_nro(wii-nx-launcher_nro TARGET wii-nx-launcher
    OUTPUT wii-nx-launcher.nro NACP wii-nx-launcher.nacp)
