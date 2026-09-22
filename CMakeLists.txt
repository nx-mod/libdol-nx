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

# accel/nw4r: natives for nw4r (lyt so far).
add_library(wiinx_nw4r STATIC
    src/accel/nw4r/nw4r.cpp
    src/accel/nw4r/lyt/pane.cpp
)
target_link_libraries(wiinx_nw4r PUBLIC wiinx_core)
add_library(wiinx::nw4r ALIAS wiinx_nw4r)

# accel/sdk: natives for the RVL SDK (THP so far).
add_library(wiinx_sdk STATIC
    src/accel/sdk/sdk.cpp
    src/accel/sdk/thp/thp_decode.cpp
)
target_link_libraries(wiinx_sdk PUBLIC wiinx_core)
add_library(wiinx::sdk ALIAS wiinx_sdk)
