# The library's checks: small programs that assert and return non-zero.
#
# They cover the sections that build anywhere, so `ctest` runs them on whatever
# machine is doing the building - no Switch, no game, no disc. Each one is a
# single file named after what it checks.
set(WIINX_CHECKS
    core_check          # typed guest memory and the native registry
    nw4r_lyt_check      # layout matrices against the original's arithmetic
    sdk_mtx_check       # the matrix library, including its paired-single order
    sdk_thp_check       # the video decoder against a known frame
    nand_sysconf_check  # SYSCONF, against a file the runtime wrote
    nand_mii_check      # the Mii database, against a file the runtime wrote
    nand_title_check    # tickets, TMDs and WADs, built here and read back
    archive_yaz0_check  # Yaz0, including the streams a game must refuse
)

foreach(check IN LISTS WIINX_CHECKS)
    add_executable(${check} ${check}.cpp)
    target_link_libraries(${check} PRIVATE wiinx::wiinx)
endforeach()

# Two of them read a reference file written by the runtime's own first-run code.
add_test(NAME core COMMAND core_check)
add_test(NAME nw4r_lyt COMMAND nw4r_lyt_check)
add_test(NAME sdk_mtx COMMAND sdk_mtx_check)
add_test(NAME sdk_thp COMMAND sdk_thp_check)
add_test(NAME nand_sysconf COMMAND nand_sysconf_check ${CMAKE_CURRENT_SOURCE_DIR}/data/SYSCONF)
add_test(NAME nand_mii COMMAND nand_mii_check ${CMAKE_CURRENT_SOURCE_DIR}/data/RFL_DB.dat)
add_test(NAME nand_title COMMAND nand_title_check)
add_test(NAME archive_yaz0 COMMAND archive_yaz0_check)
