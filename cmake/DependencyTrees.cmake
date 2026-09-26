# Where the trees this library builds against actually live.
#
# Aurora (GX on WebGPU) and Dawn are checkouts beside libdol-nx rather than parts
# of it, and their folder names are not fixed: Aurora is `aurora-main` inside the
# wiicompiled product, and Dawn is plainly `dawn`. Each place that needed one had
# guessed a single name - `aurora-nx`, `dawn-nx` - that nothing on disk is
# called, so every build that worked passed the path in by hand, and a build that
# did not got no runtime at all without being told why.
#
# Searching by name alone is not enough either, because there can be more than
# one. This checkout has two Aurora trees: `aurora`, which is plain upstream, and
# `wiicompiled/aurora-main`, which carries the Switch port. Only the ported one
# vendors its dependencies instead of looking for host packages, so only it can
# cross-compile; taking the first name that matched would have silently chosen
# the other, and the build would have failed a long way from the cause. A
# candidate is therefore judged by whether it can do the build being asked for,
# and when more than one can, the choice is named rather than left to the order
# of a list in this file.

set(WIINX_AURORA_NAMES aurora-nx aurora-main aurora)
set(WIINX_DAWN_NAMES dawn-nx dawn-main dawn)

# What tells a ported Aurora from plain upstream. Both mention
# CMAKE_CROSSCOMPILING, so that is no use as a marker; vendoring the dependencies
# is the thing that actually makes the cross build work, and only the port does
# it.
set(WIINX_AURORA_CROSS_MARKER "_default_provider \"vendor\"")

# wiinx_find_tree(<out_var> <label> <root> NAMES <name>... [CROSS_MARKER <text>])
#
# Looks for NAMES beside <root>, and inside a product checkout beside it - which
# is where a dependency vendored by a product ends up. When cross-compiling and
# CROSS_MARKER is given, a candidate whose CMakeLists.txt does not contain that
# text is rejected as unported.
#
# Sets <out_var>, and <out_var>_SEARCHED with everywhere it looked, so a caller's
# failure message can say more than "missing".
function(wiinx_find_tree out_var label root)
    cmake_parse_arguments(ARG "" "CROSS_MARKER" "NAMES" ${ARGN})

    if(${out_var})
        # Already answered, on the command line or by a parent project. Take it,
        # but do not let a wrong path become a confusing failure further in.
        if(NOT EXISTS "${${out_var}}/CMakeLists.txt")
            message(FATAL_ERROR
                "${out_var} is set to ${${out_var}}, which has no CMakeLists.txt.\n"
                "Point it at a ${label} checkout, or leave it unset to search for one.")
        endif()
        return()
    endif()

    set(searched "")
    set(usable "")
    set(unported "")
    foreach(product "" wiicompiled gccompiled-nx wii-nx)
        foreach(name ${ARG_NAMES})
            if(product STREQUAL "")
                set(candidate "${root}/../${name}")
            else()
                set(candidate "${root}/../${product}/${name}")
            endif()
            get_filename_component(candidate "${candidate}" ABSOLUTE)
            if(candidate IN_LIST searched)
                continue()
            endif()
            list(APPEND searched "${candidate}")
            if(NOT EXISTS "${candidate}/CMakeLists.txt")
                continue()
            endif()
            if(CMAKE_CROSSCOMPILING AND ARG_CROSS_MARKER)
                file(READ "${candidate}/CMakeLists.txt" text)
                string(FIND "${text}" "${ARG_CROSS_MARKER}" marker_at)
                if(marker_at LESS 0)
                    list(APPEND unported "${candidate}")
                    continue()
                endif()
            endif()
            list(APPEND usable "${candidate}")
        endforeach()
    endforeach()

    set(${out_var}_SEARCHED "${searched}" PARENT_SCOPE)
    if(NOT usable)
        if(unported)
            # Found, but none of them is ported. Saying so here is the whole
            # point: the alternative is a failure deep in a third-party build.
            message(FATAL_ERROR
                "${label} is here, but none of these is ported for cross-compiling - "
                "they do not vendor their dependencies, so they will look for host "
                "packages that a Switch build has not got:\n"
                "  ${unported}\n"
                "Point ${out_var} at a ported checkout.")
        endif()
        return()
    endif()

    list(GET usable 0 chosen)
    set(${out_var} "${chosen}" CACHE PATH "${label} source tree" FORCE)
    list(LENGTH usable count)
    if(count GREATER 1)
        message(WARNING
            "${label}: ${count} usable checkouts found, using the first:\n"
            "  ${usable}\n"
            "Set ${out_var} to choose.")
    else()
        message(STATUS "wiinx: ${label} at ${chosen}")
    endif()
endfunction()
