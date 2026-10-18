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
    src/advisor/chat_error_text.cpp
    src/advisor/chat_session.cpp
    src/advisor/freshness.cpp
    src/advisor/log_silence_monitor.cpp
    src/advisor/persona.cpp
    src/advisor/plain_words.cpp
    src/advisor/prompt_builder.cpp
    src/advisor/snapshot_renderer.cpp
    src/advisor/snapshot_store.cpp
    src/bridge/bridge_pipeline.cpp
    src/bridge/frame_assembler.cpp
    src/bridge/frame_json.cpp
    src/bridge/line_splitter.cpp
    src/bridge/log_tailer.cpp
    src/bridge/log_watcher.cpp
    src/bridge/record.cpp
    src/bridge/text_hygiene.cpp
    src/chatview/answer_text.cpp
    src/chatview/canned_conversation.cpp
    src/chatview/log_path.cpp
    src/chatview/text_layout.cpp
    src/chatview/utf8.cpp
    src/chatview/view_model.cpp
    src/chatview/view_text.cpp
    src/core/build_info.cpp
    src/core/app_controller.cpp
    src/core/app_log.cpp
    src/core/check_api_report.cpp
    src/core/cli.cpp
    src/core/dev_config.cpp
    src/core/headless_chat.cpp
    src/core/headless_commands.cpp
    src/core/json_lines.cpp
    src/core/redact.cpp
    src/core/secret.cpp
    src/core/transcript_writer.cpp
    src/core/ui_message.cpp
    src/core/ui_pacing.cpp
    src/core/watch_log.cpp
    src/net/ca_bundle.cpp
    src/net/endpoint_policy.cpp
    src/providers/cancel_token.cpp
    src/providers/sse_parser.cpp
    src/providers/anthropic_request.cpp
    src/providers/anthropic_stream.cpp
    src/providers/chat_error.cpp
    src/providers/provider_util.cpp
    ${CURIA_PLATFORM_SOURCES})
target_include_directories(curia_core PUBLIC src ${CMAKE_CURRENT_BINARY_DIR}/generated)
find_package(Threads REQUIRED)
target_link_libraries(curia_core PUBLIC nlohmann_json::nlohmann_json Threads::Threads)
# Public, so that the tests see the same value the library was built with (they branch on it).
if(CURIA_ENABLE_DEV_ENV)
    target_compile_definitions(curia_core PUBLIC CURIA_ENABLE_DEV_ENV=1)
endif()
curia_configure_target(curia_core)

# --- fuzz target: libFuzzer, Clang only (Apple's Clang ships no libFuzzer runtime) ---------------
if(CURIA_BUILD_FUZZ)
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        message(FATAL_ERROR "CURIA_BUILD_FUZZ needs Clang with libFuzzer (found ${CMAKE_CXX_COMPILER_ID}; "
                            "Apple Clang has no libFuzzer)")
    endif()
    # Coverage instrumentation for the code under test; the fuzzer runtime is linked into the target only.
    target_compile_options(curia_core PRIVATE -fsanitize=fuzzer-no-link)
    add_executable(curia_fuzz_bridge fuzz/fuzz_bridge.cpp)
    target_compile_options(curia_fuzz_bridge PRIVATE -fsanitize=fuzzer-no-link)
    target_link_options(curia_fuzz_bridge PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_libraries(curia_fuzz_bridge PRIVATE curia_core)
    curia_configure_target(curia_fuzz_bridge)

    add_executable(curia_fuzz_sse fuzz/fuzz_sse.cpp)
    target_compile_options(curia_fuzz_sse PRIVATE -fsanitize=fuzzer-no-link)
    target_link_options(curia_fuzz_sse PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_libraries(curia_fuzz_sse PRIVATE curia_core)
    curia_configure_target(curia_fuzz_sse)
endif()

# --- milestone M2 overlay probe, portable part (macOS only; throwaway spike code) -----------
if(APPLE)
    add_library(curia_probe_core STATIC
        probe/probe_combos.cpp
        probe/probe_options.cpp
        probe/probe_log.cpp)
    target_include_directories(curia_probe_core PUBLIC probe)
    curia_configure_target(curia_probe_core)
endif()

# --- the desktop app -----------------------------------------------------------
if(CURIA_BUILD_APP)
    find_package(SDL3 CONFIG REQUIRED)
    find_package(imgui CONFIG REQUIRED)
    find_package(CURL CONFIG REQUIRED)

    # curia_http: everything that needs libcurl (the net thread, the TLS policy, the Anthropic
    # provider). curia_core stays free of curl so the sanitizer and fuzz presets still build.
    add_library(curia_http STATIC
        src/http/anthropic_provider.cpp
        src/http/header_capture.cpp
        src/http/http_client.cpp
        src/http/tls_policy.cpp
        src/http/transport_error.cpp)
    target_link_libraries(curia_http PUBLIC curia_core CURL::libcurl)
    if(WIN32)
        # curl pulls in <windows.h>, whose min/max macros break std::min/std::max.
        target_compile_definitions(curia_http PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
    curia_configure_target(curia_http)

    # The headless modes of the curia executable (--headless-chat, --check-api): the shared logic is
    # in curia_core, this adds the Anthropic provider and the endpoint. No SDL.
    add_library(curia_headless STATIC
        src/headless/check_api.cpp
        src/headless/headless_modes.cpp)
    target_link_libraries(curia_headless PUBLIC curia_http)
    curia_configure_target(curia_headless)

    # The bundled font (app/resources/fonts/README.md): each TTF becomes a generated source file
    # with its bytes, rebuilt when the TTF changes, so the executable needs no file at run time.
    set(CURIA_FONT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/resources/fonts)
    set(CURIA_EMBEDDED_FONTS)
    foreach(face IN ITEMS Regular Bold)
        set(font_file ${CURIA_FONT_DIR}/GentiumBookPlus-${face}.ttf)
        set(font_source ${CMAKE_CURRENT_BINARY_DIR}/generated/embedded/gentium_book_plus_${face}.cpp)
        add_custom_command(OUTPUT ${font_source}
            COMMAND ${CMAKE_COMMAND} -DINPUT=${font_file} -DOUTPUT=${font_source}
                    -DFUNCTION=curia::ui::bundled${face}Font -DHEADER=ui/fonts.h
                    -P ${PROJECT_SOURCE_DIR}/cmake/EmbedFile.cmake
            DEPENDS ${font_file} ${PROJECT_SOURCE_DIR}/cmake/EmbedFile.cmake
            COMMENT "Embedding GentiumBookPlus-${face}.ttf"
            VERBATIM)
        list(APPEND CURIA_EMBEDDED_FONTS ${font_source})
    endforeach()

    add_executable(curia
        src/main.cpp
        src/app.cpp
        src/ui/chat_view.cpp
        src/ui/chat_window.cpp
        src/ui/sdl_renderer_backend.cpp
        src/ui/smoke_run.cpp
        src/ui/theme.cpp
        ${CURIA_EMBEDDED_FONTS})
    target_link_libraries(curia PRIVATE curia_core curia_headless SDL3::SDL3 imgui::imgui)
    curia_configure_target(curia)

    # Small tool used by CI to prove TLS works with the chosen libcurl configuration (milestone M1, A16).
    add_executable(curia_https_smoke tools/https_smoke.cpp)
    target_link_libraries(curia_https_smoke PRIVATE curia_http)
    curia_configure_target(curia_https_smoke)
    # Label "network" = required in CI (default CA handling, probed bundle only as a fallback).
    add_test(NAME https_smoke COMMAND curia_https_smoke)
    set_tests_properties(https_smoke PROPERTIES LABELS network TIMEOUT 60)
    # Label "netinfo" = informational: records what libcurl does with no help from us on each OS.
    add_test(NAME https_smoke_default_ca COMMAND curia_https_smoke --default-ca)
    set_tests_properties(https_smoke_default_ca PROPERTIES LABELS netinfo TIMEOUT 60)

    # Smoke test: the chat window with a canned conversation and a scripted answer (no network, no
    # key, no game), the bundled font checked for every character shown, the last frame saved as a
    # BMP file; then exit (software renderer, no GPU needed). Pass/fail is the process exit code.
    add_test(NAME app_smoke COMMAND curia --smoke --screenshot ${CMAKE_CURRENT_BINARY_DIR}/app_smoke.bmp)
    set_tests_properties(app_smoke PROPERTIES LABELS smoke TIMEOUT 60)

    # Idle test: the event loop must not draw frames while nothing happens (needs a display; the
    # software renderer avoids needing a GPU). Do not move the mouse over the window while it runs.
    add_test(NAME app_idle COMMAND curia --idle-test 5)
    set_tests_properties(app_idle PROPERTIES LABELS idle TIMEOUT 90
                         ENVIRONMENT "SDL_RENDER_DRIVER=software")

    if(APPLE)
        # The M2 overlay probe: an app bundle (launched with `open`, see docs/spikes/m2-checklist.md).
        add_executable(curia_m2_probe MACOSX_BUNDLE
            probe/probe_main.mm
            probe/probe_mac.mm
            src/ui/sdl_renderer_backend.cpp)
        target_link_libraries(curia_m2_probe PRIVATE curia_core curia_probe_core SDL3::SDL3 imgui::imgui
            "-framework AppKit" "-framework Carbon" "-framework CoreGraphics" "-framework AudioToolbox")
        target_compile_options(curia_m2_probe PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
        set_target_properties(curia_m2_probe PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/probe/Info.plist.in
            MACOSX_BUNDLE_BUNDLE_NAME "Curia M2 Probe"
            MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.cupskeee.curia.m2probe"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
            MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}")
        curia_configure_target(curia_m2_probe)
        # The workspace-observer path crashed once (a block captured a dead local); this posts synthetic
        # notifications after install and needs no window and no focus change.
        add_test(NAME probe_observers COMMAND $<TARGET_FILE:curia_m2_probe> --selftest-observers)
        set_tests_properties(probe_observers PROPERTIES LABELS smoke TIMEOUT 30
                             PASS_REGULAR_EXPRESSION "PROBE_SELFTEST_OK")
        # Ad-hoc signature over the whole bundle so the Info.plist is bound to it.
        add_custom_command(TARGET curia_m2_probe POST_BUILD
            COMMAND codesign --force --sign - "$<TARGET_BUNDLE_DIR:curia_m2_probe>"
            VERBATIM)
    endif()

    # A command-line client of the Anthropic provider: the test tool of the provider_mock test and the
    # owner's preflight. It prints one JSON object per event (tools/chat_probe.cpp).
    add_executable(curia_chat_probe tools/chat_probe.cpp)
    target_link_libraries(curia_chat_probe PRIVATE curia_http)
    curia_configure_target(curia_chat_probe)

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
        tests/advisor_test_support.cpp
        tests/bridge_test_support.cpp
        tests/test_advisor_monitor.cpp
        tests/test_advisor_plain_words.cpp
        tests/test_advisor_prompt.cpp
        tests/test_advisor_renderer.cpp
        tests/test_advisor_session.cpp
        tests/test_app_controller.cpp
        tests/test_bridge_assembler.cpp
        tests/test_bridge_fixtures.cpp
        tests/test_bridge_hygiene.cpp
        tests/test_bridge_json.cpp
        tests/test_bridge_parser.cpp
        tests/test_bridge_robustness.cpp
        tests/test_bridge_splitter.cpp
        tests/test_build_info.cpp
        tests/test_chatview_answer.cpp
        tests/test_chatview_layout.cpp
        tests/test_chatview_model.cpp
        tests/test_chatview_text.cpp
        tests/test_check_api_report.cpp
        tests/test_cli.cpp
        tests/test_app_log.cpp
        tests/test_ca_bundle.cpp
        tests/test_dev_config.cpp
        tests/test_endpoint_policy.cpp
        tests/test_headless_chat.cpp
        tests/test_headless_commands.cpp
        tests/test_json_lines.cpp
        tests/test_redact.cpp
        tests/test_secret.cpp
        tests/test_transcript_writer.cpp
        tests/test_log_tailer.cpp
        tests/test_log_watcher.cpp
        tests/test_mailbox.cpp
        tests/test_platform.cpp
        tests/test_provider_types.cpp
        tests/test_sse_parser.cpp
        tests/test_watch_log.cpp
        tests/test_anthropic_request.cpp
        tests/test_anthropic_stream.cpp
        tests/test_chat_error.cpp
        tests/test_provider_util.cpp)
    if(APPLE)
        target_sources(curia_tests PRIVATE
            tests/test_probe_combos.cpp
            tests/test_probe_log.cpp
            tests/test_probe_options.cpp)
        target_link_libraries(curia_tests PRIVATE curia_probe_core)
    endif()
    target_link_libraries(curia_tests PRIVATE curia_core doctest::doctest)
    # The committed CURIA1 fixtures and their golden outputs (tests/fixtures/curia1/README.md).
    target_compile_definitions(curia_tests PRIVATE
        CURIA_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures"
        CURIA_ANTHROPIC_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures/anthropic"
        CURIA_GOLDEN_DIR="${PROJECT_SOURCE_DIR}/tests/golden")
    curia_configure_target(curia_tests)
    include(doctest)
    doctest_discover_tests(curia_tests PROPERTIES LABELS unit)

    # The fuzz entry point (fuzz/fuzz_bridge.cpp) behind a plain main(): replays the fixtures and the
    # seed corpus without libFuzzer, on every platform and compiler.
    add_executable(curia_fuzz_replay fuzz/fuzz_bridge.cpp fuzz/fuzz_replay_main.cpp)
    target_link_libraries(curia_fuzz_replay PRIVATE curia_core)
    curia_configure_target(curia_fuzz_replay)
    add_test(NAME fuzz_replay
             COMMAND curia_fuzz_replay ${PROJECT_SOURCE_DIR}/tests/fixtures/curia1
                     ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/corpus)
    set_tests_properties(fuzz_replay PROPERTIES LABELS fuzz TIMEOUT 300)

    # The same for the SSE parser (fuzz/fuzz_sse.cpp, corpus in fuzz/corpus_sse).
    add_executable(curia_fuzz_sse_replay fuzz/fuzz_sse.cpp fuzz/fuzz_sse_replay_main.cpp)
    target_link_libraries(curia_fuzz_sse_replay PRIVATE curia_core)
    curia_configure_target(curia_fuzz_sse_replay)
    add_test(NAME fuzz_replay_sse
             COMMAND curia_fuzz_sse_replay ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/corpus_sse)
    set_tests_properties(fuzz_replay_sse PROPERTIES LABELS fuzz TIMEOUT 300)
endif()

# --- end-to-end tests (need the app binaries and Python 3) ----------------------------------------
# Python 3 is optional unless CURIA_REQUIRE_PYTHON is ON (CI): then a missing interpreter fails the
# configuration instead of silently dropping these tests.
if(CURIA_BUILD_APP AND CURIA_BUILD_TESTS)
    find_package(Python3 COMPONENTS Interpreter)
    if(NOT Python3_Interpreter_FOUND AND CURIA_REQUIRE_PYTHON)
        message(FATAL_ERROR "CURIA_REQUIRE_PYTHON is ON but no Python 3 interpreter was found")
    endif()

    # Unit tests of curia_http (TLS policy decisions, header parsing, transfer rules, the client's
    # lifecycle). A separate executable and a separate folder: curia_tests must not link libcurl,
    # and the clang-tidy job of CI (core configuration, no libcurl) walks app/tests.
    add_executable(curia_http_tests
        tests/test_main.cpp
        http_tests/test_anthropic_provider.cpp
        http_tests/test_header_capture.cpp
        http_tests/test_http_client.cpp
        http_tests/test_tls_policy.cpp
        http_tests/test_transport_error.cpp)
    target_link_libraries(curia_http_tests PRIVATE curia_http doctest::doctest)
    curia_configure_target(curia_http_tests)
    doctest_discover_tests(curia_http_tests PROPERTIES LABELS unit)

    if(Python3_Interpreter_FOUND)
        # fake CK3 -> curia --watch-log -> golden events. The chain with the mock LLM is chat_e2e below.
        add_test(NAME e2e_fake_ck3
                 COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/fake_ck3/e2e_test.py
                         --curia $<TARGET_FILE:curia>
                         --fixtures ${PROJECT_SOURCE_DIR}/tests/fixtures/curia1)
        set_tests_properties(e2e_fake_ck3 PROPERTIES LABELS e2e TIMEOUT 300)

        # The Anthropic provider against the mock server (tools/mock_llm): every stream shape, every
        # HTTP status, stall, cancel with a socket-close check, key secrecy, no redirect. Needs the
        # development configuration (a key from the environment and --test-endpoint).
        if(CURIA_ENABLE_DEV_ENV)
            add_test(NAME provider_mock
                     COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/provider_mock.py
                             --probe $<TARGET_FILE:curia_chat_probe>
                             --mock ${PROJECT_SOURCE_DIR}/tools/mock_llm/mock_llm.py)
            set_tests_properties(provider_mock PROPERTIES LABELS "e2e;provider" TIMEOUT 300)

            # fake CK3 -> curia --headless-chat -> mock LLM, and the same chain with a random fake key
            # that must appear nowhere (tests/chat_e2e.py).
            add_test(NAME chat_e2e
                     COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/chat_e2e.py
                             --suite chat --curia $<TARGET_FILE:curia>
                             --mock ${PROJECT_SOURCE_DIR}/tools/mock_llm/mock_llm.py
                             --fake-ck3 ${PROJECT_SOURCE_DIR}/tools/fake_ck3/fake_ck3.py)
            set_tests_properties(chat_e2e PROPERTIES LABELS "e2e;chat" TIMEOUT 600)
            add_test(NAME e2e_secrecy
                     COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/chat_e2e.py
                             --suite secrecy --curia $<TARGET_FILE:curia>
                             --mock ${PROJECT_SOURCE_DIR}/tools/mock_llm/mock_llm.py
                             --fake-ck3 ${PROJECT_SOURCE_DIR}/tools/fake_ck3/fake_ck3.py)
            set_tests_properties(e2e_secrecy PROPERTIES LABELS "e2e;secrecy" TIMEOUT 600)
        else()
            message(STATUS "CURIA_ENABLE_DEV_ENV is OFF: the end-to-end tests provider_mock, chat_e2e and e2e_secrecy are not registered")
        endif()
    else()
        message(STATUS "Python 3 not found: the end-to-end tests e2e_fake_ck3, provider_mock, chat_e2e and e2e_secrecy are not registered")
    endif()
endif()
