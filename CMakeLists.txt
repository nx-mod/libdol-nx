cmake_minimum_required(VERSION 3.20)
project(libwii-nx LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# core: types, typed guest access, the host interface, the native registry.
add_library(wiinx_core STATIC
    src/core/host.cpp
    src/core/native.cpp
)
target_include_directories(wiinx_core PUBLIC include)
add_library(wiinx::core ALIAS wiinx_core)
