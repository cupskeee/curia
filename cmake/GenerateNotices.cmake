# SPDX-License-Identifier: MIT
# Generates THIRD_PARTY_NOTICES.txt from the license files vcpkg installs for each linked port.
# Run as a script:
#   cmake -DINSTALLED_DIR=<build>/vcpkg_installed/<triplet> -DOUTPUT=<file> -P cmake/GenerateNotices.cmake
# Entries that vcpkg cannot know (an embedded font, a bundled CA bundle) are appended by hand in
# cmake/extra_notices.txt when they exist. Used at packaging time (milestone M7).
if(NOT INSTALLED_DIR OR NOT OUTPUT)
    message(FATAL_ERROR "Usage: cmake -DINSTALLED_DIR=<dir> -DOUTPUT=<file> -P GenerateNotices.cmake")
endif()

file(GLOB port_dirs LIST_DIRECTORIES true "${INSTALLED_DIR}/share/*")
list(SORT port_dirs)

set(text "THIRD-PARTY NOTICES\n\nCuria is MIT licensed (see LICENSE). It links the following third-party\ncomponents, each under its own license, reproduced below as installed by vcpkg.\n")
foreach(dir IN LISTS port_dirs)
    get_filename_component(port "${dir}" NAME)
    if(port MATCHES "^vcpkg-" OR port STREQUAL "doctest" OR NOT EXISTS "${dir}/copyright")
        continue()
    endif()
    file(READ "${dir}/copyright" license_text)
    string(APPEND text "\n==============================================================================\n${port}\n==============================================================================\n${license_text}\n")
endforeach()

get_filename_component(here "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)
if(EXISTS "${here}/extra_notices.txt")
    file(READ "${here}/extra_notices.txt" extra)
    string(APPEND text "\n${extra}\n")
endif()

file(WRITE "${OUTPUT}" "${text}")
message(STATUS "Wrote ${OUTPUT}")
