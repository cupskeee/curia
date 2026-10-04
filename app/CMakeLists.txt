# SPDX-License-Identifier: MIT
find_package(nlohmann_json CONFIG REQUIRED)

# --- shared compile settings -------------------------------------------------
function(curia_configure_target target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
        if(CURIA_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion)
        if(CURIA_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
    if(CURIA_SANITIZE AND NOT MSVC)
        target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer
                                                 -fno-sanitize-recover=undefined)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
endfunction()

# --- curia_core: everything that needs no windowing, UI or networking --------
if(WIN32)
    set(CURIA_PLATFORM_SOURCES src/platform/win/platform_win.cpp)
elseif(APPLE)
    set(CURIA_PLATFORM_SOURCES src/platform/mac/platform_mac.cpp)
else()
    set(CURIA_PLATFORM_SOURCES src/platform/linux/platform_linux.cpp)
endif()

configure_file(src/core/version.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/curia/version.h @ONLY)

add_library(curia_core STATIC
    src/core/build_info.cpp
    src/core/cli.cpp
    src/net/ca_bundle.cpp
    ${CURIA_PLATFORM_SOURCES})
target_include_directories(curia_core PUBLIC src ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(curia_core PUBLIC nlohmann_json::nlohmann_json)
curia_configure_target(curia_core)

# --- the desktop app -----------------------------------------------------------
if(CURIA_BUILD_APP)
    find_package(SDL3 CONFIG REQUIRED)
    find_package(imgui CONFIG REQUIRED)
    find_package(CURL CONFIG REQUIRED)

    add_executable(curia
        src/main.cpp
        src/app.cpp
        src/ui/sdl_renderer_backend.cpp)
    target_link_libraries(curia PRIVATE curia_core SDL3::SDL3 imgui::imgui)
    curia_configure_target(curia)

    # Small tool used by CI to prove TLS works with the chosen libcurl configuration (milestone M1, A16).
    add_executable(curia_https_smoke tools/https_smoke.cpp)
    target_link_libraries(curia_https_smoke PRIVATE curia_core CURL::libcurl)
    curia_configure_target(curia_https_smoke)
    # Label "network" = required in CI (default CA handling, probed bundle only as a fallback).
    add_test(NAME https_smoke COMMAND curia_https_smoke)
    set_tests_properties(https_smoke PROPERTIES LABELS network TIMEOUT 60)
    # Label "netinfo" = informational: records what libcurl does with no help from us on each OS.
    add_test(NAME https_smoke_default_ca COMMAND curia_https_smoke --default-ca)
    set_tests_properties(https_smoke_default_ca PROPERTIES LABELS netinfo TIMEOUT 60)

    # Headless smoke test: window + renderer + 3 frames, then exit (software renderer, no GPU needed).
    # Pass/fail is the process exit code.
    add_test(NAME app_smoke COMMAND curia --smoke)
    set_tests_properties(app_smoke PROPERTIES LABELS smoke TIMEOUT 60)

    # Idle test: the event loop must not draw frames while nothing happens (needs a display; the
    # software renderer avoids needing a GPU). Do not move the mouse over the window while it runs.
    add_test(NAME app_idle COMMAND curia --idle-test 5)
    set_tests_properties(app_idle PROPERTIES LABELS idle TIMEOUT 90
                         ENVIRONMENT "SDL_RENDER_DRIVER=software")

    # Third-party license notices for packaging: cmake --build <dir> --target curia_notices
    add_custom_target(curia_notices
        COMMAND ${CMAKE_COMMAND} -DINSTALLED_DIR=${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}
                -DOUTPUT=${CMAKE_BINARY_DIR}/THIRD_PARTY_NOTICES.txt
                -P ${PROJECT_SOURCE_DIR}/cmake/GenerateNotices.cmake
        COMMENT "Generating THIRD_PARTY_NOTICES.txt")
endif()

# --- tests ------------------------------------------------------------------------
if(CURIA_BUILD_TESTS)
    find_package(doctest CONFIG REQUIRED)
    add_executable(curia_tests
        tests/test_main.cpp
        tests/test_build_info.cpp
        tests/test_cli.cpp
        tests/test_ca_bundle.cpp
        tests/test_platform.cpp)
    target_link_libraries(curia_tests PRIVATE curia_core doctest::doctest)
    curia_configure_target(curia_tests)
    include(doctest)
    doctest_discover_tests(curia_tests PROPERTIES LABELS unit)
endif()
