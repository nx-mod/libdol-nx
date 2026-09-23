cmake_minimum_required(VERSION 3.20)
project(libwii-nx LANGUAGES C CXX)

set(WIINX_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")

# core: types, typed guest access, the host interface, the native registry.
add_library(wiinx_core STATIC
    src/core/host.cpp
    src/core/native.cpp
)
target_include_directories(wiinx_core PUBLIC include)
target_compile_features(wiinx_core PUBLIC cxx_std_20)
add_library(wiinx::core ALIAS wiinx_core)

# accel/nw4r: natives for nw4r (lyt so far).
add_library(wiinx_nw4r STATIC
    src/accel/nw4r/nw4r.cpp
    src/accel/nw4r/lyt/pane.cpp
)
target_link_libraries(wiinx_nw4r PUBLIC wiinx_core)
add_library(wiinx::nw4r ALIAS wiinx_nw4r)

# accel/sdk: natives for the RVL SDK (THP video, the matrix library).
add_library(wiinx_sdk STATIC
    src/accel/sdk/sdk.cpp
    src/accel/sdk/mtx/mtx.cpp
    src/accel/sdk/thp/thp_decode.cpp
)
target_link_libraries(wiinx_sdk PUBLIC wiinx_core)
add_library(wiinx::sdk ALIAS wiinx_sdk)

# The CPU runtime, the platform and the app: they need Aurora (GX on WebGPU)
# and a game's translated code. A build that has Aurora already (the Switch
# build adds it first) or points WIINX_AURORA_DIR at it gets them.
set(WIINX_AURORA_DIR "${WIINX_ROOT}/../aurora-nx" CACHE PATH "Aurora (aurora-nx) source tree")
if(TARGET aurora_core OR EXISTS "${WIINX_AURORA_DIR}/CMakeLists.txt")
    set(WIINX_RUNTIME_DEFAULT ON)
else()
    set(WIINX_RUNTIME_DEFAULT OFF)
endif()
option(WIINX_RUNTIME "Build the CPU runtime, the platform and the app" ${WIINX_RUNTIME_DEFAULT})

if(WIINX_RUNTIME)
    # Headers the runtime and translated game code include by bare name, one
    # folder per layer, plus libwii-nx's public headers.
    set(WIINX_RUNTIME_INCLUDE_DIRS
        "${WIINX_ROOT}/src/cpu/include"
        "${WIINX_ROOT}/src/platform/include"
        "${WIINX_ROOT}/src/app/include"
        "${WIINX_ROOT}/include")
    include("${WIINX_ROOT}/cmake/runtime/Runtime.cmake")
    # The runtime is an object library, consumed as objects rather than linked,
    # so its usage requirements do not reach the executable: link the native
    # modules into each product instead.
    foreach(wiinx_product WiiCompiled RetroRewind)
        if(TARGET ${wiinx_product})
            target_link_libraries(${wiinx_product} PRIVATE wiinx::sdk wiinx::nw4r wiinx::core)
        endif()
    endforeach()
endif()
