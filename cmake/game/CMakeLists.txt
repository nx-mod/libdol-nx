# Builds one game: the pieces in order, ending in <game>.nro laid out for
# sdmc:/wii-nx/games/<game>/. tools/wiinx-build drives this; it holds no logic
# of its own beyond the order things are added in.
#
#   cmake -S cmake/game -B <game>/build -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake \
#         -DWIINX_GAME_DIR=<game> [-DWIINX_DAWN_DIR=<dawn>] [-DWIINX_AURORA_DIR=<aurora>]
# Build the game, not everything beside it:
#
#   cmake --build <dir> --target <game>_nro
#
# A plain `cmake --build` compiles every target these third-party projects
# define, including ones nothing here links - Aurora's desktop application
# layer, Abseil's signal handler - and those are written for a desktop libc
# rather than for this one. The game's own target pulls in exactly what it
# needs.
cmake_minimum_required(VERSION 3.22)

# Release is the only configuration the runtime supports, and what its flags
# assume. Set before project() so it reaches the compiler.
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif ()

project(wiinx-game LANGUAGES C CXX ASM)

if (NOT CMAKE_SYSTEM_NAME STREQUAL "NintendoSwitch")
    message(FATAL_ERROR
        "libdol-nx builds for Nintendo Switch. Configure with:\n"
        "  -DCMAKE_TOOLCHAIN_FILE=\$DEVKITPRO/cmake/Switch.cmake")
endif ()

get_filename_component(WIINX_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(WIINX_GAME_DIR "" CACHE PATH "The game project to build")
include("${WIINX_ROOT}/cmake/DependencyTrees.cmake")
set(WIINX_DAWN_DIR "" CACHE PATH "Dawn checkout; unset searches for one")
set(WIINX_AURORA_DIR "" CACHE PATH "Aurora checkout; unset searches for one")
wiinx_find_tree(WIINX_DAWN_DIR "Dawn" "${WIINX_ROOT}" NAMES ${WIINX_DAWN_NAMES})
wiinx_find_tree(WIINX_AURORA_DIR "Aurora" "${WIINX_ROOT}"
    NAMES ${WIINX_AURORA_NAMES} CROSS_MARKER "${WIINX_AURORA_CROSS_MARKER}")

if (NOT EXISTS "${WIINX_GAME_DIR}/recomp.yml")
    message(FATAL_ERROR "No game project at ${WIINX_GAME_DIR} (-DWIINX_GAME_DIR=<folder>)")
endif ()
if (NOT EXISTS "${WIINX_GAME_DIR}/generated/build_shards/shards.cmake")
    message(FATAL_ERROR
        "${WIINX_GAME_DIR} has no translated code yet. Translate it first:\n"
        "  tools/wiinx-translate ${WIINX_GAME_DIR}")
endif ()
foreach (piece DAWN AURORA)
    if (NOT EXISTS "${WIINX_${piece}_DIR}/CMakeLists.txt")
        # Say where it looked. The old message named only the one path it had
        # guessed, which was a folder name nothing on disk uses.
        message(FATAL_ERROR
            "${piece} is not here, and a game is built against it "
            "(-DWIINX_${piece}_DIR=<checkout>).\n"
            "Looked in: ${WIINX_${piece}_DIR_SEARCHED}${WIINX_${piece}_DIR}")
    endif ()
endforeach ()
get_filename_component(WIINX_GAME_NAME "${WIINX_GAME_DIR}" NAME)

# WebGPU. Dawn builds a game itself when it is the top-level project; here it is
# not, so it builds only itself.
set(DAWN_BUILD_MONOLITHIC_LIBRARY STATIC CACHE STRING "" FORCE)
set(DAWN_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(DAWN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(DAWN_BUILD_PROTOBUF OFF CACHE BOOL "" FORCE)
set(DAWN_FETCH_DEPENDENCIES OFF CACHE BOOL "" FORCE)
set(DAWN_ENABLE_VULKAN ON CACHE BOOL "" FORCE)
set(DAWN_USE_GLFW OFF CACHE BOOL "" FORCE)
set(DAWN_USE_X11 OFF CACHE BOOL "" FORCE)
set(DAWN_USE_WAYLAND OFF CACHE BOOL "" FORCE)
set(TINT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(TINT_BUILD_CMD_TOOLS OFF CACHE BOOL "" FORCE)
# Dawn is the same library whichever game is being built, and nothing here
# compiles against its headers - it is linked and nothing else. Building it
# again per game costs 4.9 GB and about an hour, so a build can point at one
# that already exists and link that.
#
#   -DWIINX_DAWN_PREBUILT=<a build directory that has one>
#
# Left unset, it is built here as before.
set(WIINX_DAWN_PREBUILT "" CACHE PATH "A Dawn build to link, rather than building one")
set(_wiinx_dawn_library "${WIINX_DAWN_PREBUILT}/src/dawn/native/libwebgpu_dawn.a")
if (WIINX_DAWN_PREBUILT AND EXISTS "${_wiinx_dawn_library}")
    add_library(webgpu_dawn STATIC IMPORTED GLOBAL)
    set_target_properties(webgpu_dawn PROPERTIES
        IMPORTED_LOCATION "${_wiinx_dawn_library}"
        INTERFACE_INCLUDE_DIRECTORIES
            "${WIINX_DAWN_DIR}/include;${WIINX_DAWN_PREBUILT}/gen/include")
    # Aurora asks for it under Dawn's own namespace, which a from-source build
    # gets from Dawn's CMakeLists; nothing defines it when that never runs.
    add_library(dawn::webgpu_dawn ALIAS webgpu_dawn)
    # Aurora links absl::btree and absl::flat_hash_map, and takes them as given
    # when they already exist - which they do when Dawn is built from source,
    # because Dawn builds its own abseil. Without them Aurora fetches one and
    # builds it, and abseil does not compile for Switch (pthread_t is not a
    # pid_t there). They are header-only, and the little they do compile is
    # already inside libwebgpu_dawn.a, so its headers are the whole of it.
    foreach (_wiinx_absl btree flat_hash_map)
        add_library(absl::${_wiinx_absl} INTERFACE IMPORTED GLOBAL)
        set_target_properties(absl::${_wiinx_absl} PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${WIINX_DAWN_DIR}/third_party/abseil-cpp")
    endforeach ()
    message(STATUS "libdol-nx: linking the Dawn already built in ${WIINX_DAWN_PREBUILT}")
elseif (WIINX_DAWN_PREBUILT)
    message(FATAL_ERROR
        "No Dawn in ${WIINX_DAWN_PREBUILT}: expected ${_wiinx_dawn_library}")
else ()
    add_subdirectory("${WIINX_DAWN_DIR}" dawn-build)
endif ()

# Aurora (GX on WebGPU) reuses the Dawn added above rather than fetching its
# own; its CMake checks for the target before doing so.
set(AURORA_ENABLE_GX ON CACHE BOOL "" FORCE)
set(AURORA_ENABLE_DVD OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_CARD OFF CACHE BOOL "" FORCE)
set(AURORA_DAWN_PROVIDER "vendor" CACHE STRING "" FORCE)
set(AURORA_SDL3_PROVIDER "vendor" CACHE STRING "" FORCE)
# EXCLUDE_FROM_ALL, as the runtime adds it: only the targets something links
# are built. aurora_core is the desktop application layer - a window, an event
# loop, SDL - and a Switch build has its own, so nothing here links it and
# nothing here needs SDL3.
add_subdirectory("${WIINX_AURORA_DIR}" aurora-build EXCLUDE_FROM_ALL)

# Every game binds its natives through its own table (see native_bindings.h),
# so there is nothing here to decide: no game's addresses are privileged.

# Which folder on the card this game reads and writes: sdmc:/wii-nx/games/<name>.
#
# Every target has to agree. It used to be set on the executable alone, and the
# code that actually resolves paths lives in the platform and the app - so a
# second game wrote its logs, its saves and its shader cache into the first
# game's folder, and switch_layout.h's default is what it used.
add_compile_definitions(WIINX_GAME_DIR="${WIINX_GAME_NAME}")

# Which shelf the title is on, so its config, caches and logs land beside its
# NRO rather than always under games/. A project kept in one of the three
# shelves says so by where it is; one kept anywhere else - next to a libdol-nx
# checkout, say - is a game.
get_filename_component(_wiinx_shelf "${WIINX_GAME_DIR}" DIRECTORY)
get_filename_component(_wiinx_shelf "${_wiinx_shelf}" NAME)
if (NOT _wiinx_shelf MATCHES "^(games|wads|titles)$")
    set(_wiinx_shelf "games")
endif ()
add_compile_definitions(WIINX_GAME_SHELF="${_wiinx_shelf}")

# Where the guest starts. It is written into the generated RuntimeConfig.h too,
# but that header is not on every source's include path - main.cpp cannot see
# it - and a fallback that quietly uses another game's entry is the very
# failure this fixes. A definition reaches every translation unit.
file(READ "${WIINX_GAME_DIR}/recomp.yml" _wiinx_entry_project)
if (_wiinx_entry_project MATCHES "entry_points:[ \t\r\n]*-[ \t]*(0[xX][0-9A-Fa-f]+)")
    add_compile_definitions(WIINX_GUEST_ENTRY=${CMAKE_MATCH_1}u)
    message(STATUS "libdol-nx: the guest starts at ${CMAKE_MATCH_1}")
else ()
    message(FATAL_ERROR
        "No entry point in ${WIINX_GAME_DIR}/recomp.yml (translation.entry_points). "
        "Without it the runtime would start at another game's entry.")
endif ()
message(STATUS "libdol-nx: ${WIINX_GAME_NAME} keeps its data in ${_wiinx_shelf}/")

# libdol-nx: the CPU runtime, the console, the app, and this game's code.
set(MKW_BUILD_PRODUCTS ON CACHE BOOL "" FORCE)
set(MKW_GAME_WORKSPACE_DIR "${WIINX_GAME_DIR}" CACHE PATH "" FORCE)
add_subdirectory("${WIINX_ROOT}" libdol-nx-build)

if (TARGET WiiCompiled)
    # The graphics path ends at NVK; dawn-nx knows how it has to be linked.
    include("${WIINX_DAWN_DIR}/switch_smoke_test/NvkLink.cmake")
    dawn_switch_link_nvk(WiiCompiled)
    # Every thread gets a 4 MiB stack instead of libnx's 128 KiB default: the
    # shader compiler overflows the default.
    target_link_libraries(WiiCompiled PRIVATE -Wl,--wrap=pthread_create)

    # The folder the game is copied to on the SD card names it, so the runtime
    # finds its own config, disc, caches and logs.

    # The project, for the name and region below. The read used to happen higher
    # up for the address-alias switch; that went, and took this with it.
    file(READ "${WIINX_GAME_DIR}/recomp.yml" _wiinx_project)

    # What to call this on screen. The disc's own name is not it: internal names
    # run words together ("MegaMan9", "MarioKartWii"), so a project spells the
    # real one out, and the region comes off the fourth character of the disc id
    # the way every catalogue writes it.
    set(_wiinx_title "${WIINX_GAME_NAME}")
    if (_wiinx_project MATCHES "\n[ \t]*title:[ \t]*([^\r\n#]+)")
        string(STRIP "${CMAKE_MATCH_1}" _wiinx_title)
    elseif (_wiinx_project MATCHES "\n[ \t]*display_name:[ \t]*([^\r\n#]+)")
        string(STRIP "${CMAKE_MATCH_1}" _wiinx_title)
    endif ()

    set(_wiinx_region "")
    if (_wiinx_project MATCHES "\n[ \t]*game_id:[ \t]*([A-Za-z0-9]+)")
        string(SUBSTRING "${CMAKE_MATCH_1}" 3 1 _wiinx_region_letter)
        string(TOUPPER "${_wiinx_region_letter}" _wiinx_region_letter)
        if (_wiinx_region_letter STREQUAL "E")
            set(_wiinx_region "USA")
        elseif (_wiinx_region_letter STREQUAL "P")
            set(_wiinx_region "Europe")
        elseif (_wiinx_region_letter STREQUAL "J")
            set(_wiinx_region "Japan")
        elseif (_wiinx_region_letter STREQUAL "K")
            set(_wiinx_region "Korea")
        elseif (_wiinx_region_letter STREQUAL "A")
            set(_wiinx_region "World")
        endif ()
    endif ()

    set(_wiinx_display "${_wiinx_title}")
    if (_wiinx_region)
        set(_wiinx_display "${_wiinx_title} (${_wiinx_region})")
    endif ()
    message(STATUS "libdol-nx: shown as \"${_wiinx_display}\"")

    nx_generate_nacp(${WIINX_GAME_NAME}.nacp
        NAME "${_wiinx_display}" AUTHOR "nx-mod" VERSION "1.0.0")
    nx_create_nro(${WIINX_GAME_NAME}_nro TARGET WiiCompiled
        OUTPUT ${WIINX_GAME_NAME}.nro NACP ${WIINX_GAME_NAME}.nacp)
    message(STATUS "libdol-nx: ${WIINX_GAME_NAME} -> ${WIINX_GAME_NAME}.nro")
endif ()
