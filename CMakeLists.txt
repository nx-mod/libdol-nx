# libdol-nx: the GameCube and the Wii, as one library.
#
# Six sections, each a target of its own. The three that depend on nothing but
# the standard library - core, accel and format - build on any machine and are
# what a tool, a test or a launcher links. The other three - cpu, platform and
# app - are the running console: they need Aurora and a game's translated code,
# so they are built only when those are present.
#
#   cmake -S . -B build -G Ninja      # core + accel + format, anywhere
#   cmake --build build
#
#   cmake -S . -B build-switch -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake   # everything
cmake_minimum_required(VERSION 3.20)
project(libdol-nx LANGUAGES C CXX VERSION 2.0.0)

set(WIINX_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")

# Every section's headers live under include/wiinx/<section>/, and every target
# below carries that folder as a usage requirement.
function(wiinx_section target)
    target_include_directories(${target} PUBLIC
        $<BUILD_INTERFACE:${WIINX_ROOT}/include>
        $<INSTALL_INTERFACE:include>)
    target_compile_features(${target} PUBLIC cxx_std_20)
endfunction()

# ---------------------------------------------------------------------------
# core - types, typed guest access, the host interface, the native registry.
# ---------------------------------------------------------------------------
add_library(wiinx_core STATIC
    src/core/host.cpp
    src/core/native.cpp
)
wiinx_section(wiinx_core)
add_library(wiinx::core ALIAS wiinx_core)

# ---------------------------------------------------------------------------
# format - the Wii's own file formats: NAND and discs. No host, no runtime.
# ---------------------------------------------------------------------------
add_library(wiinx_format STATIC
    src/format/nand/isfs.cpp
    src/format/nand/mii.cpp
    src/format/nand/sysconf.cpp
    src/format/nand/title.cpp
    src/format/nand/wad.cpp
    src/format/nand/store.cpp
    src/format/media/thp.cpp
    src/format/archive/yaz0.cpp
    src/format/archive/u8.cpp
    src/format/archive/cx.cpp
    src/format/disc/image.cpp
    src/format/disc/container.cpp
    src/format/disc/rvz.cpp
)
wiinx_section(wiinx_format)
add_library(wiinx::format ALIAS wiinx_format)

# ---------------------------------------------------------------------------
# accel - native versions of the libraries a game links. One target per
# library, so a program links the ones its game uses.
# ---------------------------------------------------------------------------
add_library(wiinx_sdk STATIC
    src/accel/sdk/sdk.cpp
    src/accel/sdk/msl/msl.cpp
    src/accel/sdk/mtx/mtx.cpp
    src/accel/sdk/thp/thp.cpp
)
target_link_libraries(wiinx_sdk PUBLIC wiinx_core wiinx_format)
wiinx_section(wiinx_sdk)
add_library(wiinx::sdk ALIAS wiinx_sdk)

add_library(wiinx_nw4r STATIC
    src/accel/nw4r/nw4r.cpp
    src/accel/nw4r/lyt/pane.cpp
)
target_link_libraries(wiinx_nw4r PUBLIC wiinx_core)
wiinx_section(wiinx_nw4r)
add_library(wiinx::nw4r ALIAS wiinx_nw4r)

add_library(wiinx_egg STATIC
    src/accel/egg/egg.cpp
    src/accel/egg/decomp.cpp
)
target_link_libraries(wiinx_egg PUBLIC wiinx_core wiinx_format)
wiinx_section(wiinx_egg)
add_library(wiinx::egg ALIAS wiinx_egg)

# Everything that builds anywhere, for a consumer that wants the lot.
add_library(wiinx_all INTERFACE)
target_link_libraries(wiinx_all INTERFACE
    wiinx_core wiinx_format wiinx_sdk wiinx_nw4r wiinx_egg)
add_library(wiinx::wiinx ALIAS wiinx_all)

# ---------------------------------------------------------------------------
# cpu, platform, app - the running console. They need Aurora (GX on WebGPU) and
# a game's translated code. A build that has Aurora already (the Switch build
# adds it first) or points WIINX_AURORA_DIR at it gets them.
# ---------------------------------------------------------------------------
set(WIINX_AURORA_DIR "${WIINX_ROOT}/../aurora-nx" CACHE PATH "Aurora (aurora-nx) source tree")
if(TARGET aurora_core OR EXISTS "${WIINX_AURORA_DIR}/CMakeLists.txt")
    set(WIINX_RUNTIME_DEFAULT ON)
else()
    set(WIINX_RUNTIME_DEFAULT OFF)
endif()
option(WIINX_RUNTIME "Build the CPU runtime, the platform and the app" ${WIINX_RUNTIME_DEFAULT})

if(WIINX_RUNTIME)
    # The runtime and translated game code include their own headers by bare
    # name, one folder per section, plus the SDK's public headers.
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
            target_link_libraries(${wiinx_product} PRIVATE
                wiinx::sdk wiinx::nw4r wiinx::egg wiinx::format wiinx::core)
        endif()
    endforeach()
endif()

# ---------------------------------------------------------------------------
# Tools that are programs rather than scripts. They run on the machine doing the
# building, so they are built in a host build and left out of a Switch one.
# ---------------------------------------------------------------------------
option(WIINX_TOOLS "Build the host tools" ${PROJECT_IS_TOP_LEVEL})
if(WIINX_TOOLS AND NOT CMAKE_SYSTEM_NAME STREQUAL "NintendoSwitch")
    add_executable(wiinx-disc tools/src/wiinx_disc.cpp)
    target_link_libraries(wiinx-disc PRIVATE wiinx::format)

    # Zstandard, for the chunks RVZ and WIA are compressed with. Without it the
    # tool still reads everything that is stored plainly.
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(ZSTD QUIET libzstd)
    endif()
    if(ZSTD_FOUND)
        target_compile_definitions(wiinx-disc PRIVATE WIINX_HAVE_ZSTD)
        target_include_directories(wiinx-disc PRIVATE ${ZSTD_INCLUDE_DIRS})
        target_link_libraries(wiinx-disc PRIVATE ${ZSTD_LIBRARIES})
    endif()
endif()

# ---------------------------------------------------------------------------
# Checks. They run on the machine doing the building, against the sections that
# build anywhere: ctest in a host build, nothing in a Switch one.
# ---------------------------------------------------------------------------
option(WIINX_TESTS "Build the library's checks" ${PROJECT_IS_TOP_LEVEL})
if(WIINX_TESTS AND NOT CMAKE_SYSTEM_NAME STREQUAL "NintendoSwitch")
    enable_testing()
    add_subdirectory(tests)
endif()
