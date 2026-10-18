# SPDX-License-Identifier: MIT
# Turns a binary file into a C++ source file that defines one function returning its bytes, so that a
# resource (the bundled font) is part of the executable and never read from disk at run time.
# Run as a script (app/CMakeLists.txt does it at build time, again whenever the input changes):
#   cmake -DINPUT=<file> -DOUTPUT=<file.cpp> -DFUNCTION=<qualified name> -DHEADER=<include> -P cmake/EmbedFile.cmake
# The generated file includes HEADER, which must declare
#   curia::ui::EmbeddedBytes <FUNCTION>();
# The bytes are a brace-initialised unsigned char array, not a string literal: MSVC limits a string
# literal to about 64 KB, a font is close to 1 MB.
if(NOT INPUT OR NOT OUTPUT OR NOT FUNCTION OR NOT HEADER)
    message(FATAL_ERROR "Usage: cmake -DINPUT=<file> -DOUTPUT=<file.cpp> -DFUNCTION=<name> -DHEADER=<include> -P EmbedFile.cmake")
endif()
if(NOT EXISTS "${INPUT}")
    message(FATAL_ERROR "EmbedFile: ${INPUT} does not exist")
endif()

file(READ "${INPUT}" hex HEX)
string(LENGTH "${hex}" hex_length)
math(EXPR size "${hex_length} / 2")
if(size EQUAL 0)
    message(FATAL_ERROR "EmbedFile: ${INPUT} is empty")
endif()

# Every byte becomes "0xNN,", then a line break after every 24 bytes (CMake's regular expressions
# have no {n} counts, so the pattern is spelled out).
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
set(line_pattern "")
foreach(i RANGE 1 24)
    string(APPEND line_pattern "0x[0-9a-f][0-9a-f],")
endforeach()
string(REGEX REPLACE "(${line_pattern})" "\\1\n" bytes "${bytes}")

get_filename_component(input_name "${INPUT}" NAME)
set(text "// Generated at build time by cmake/EmbedFile.cmake from ${input_name}. Do not edit.\n")
string(APPEND text "#include \"${HEADER}\"\n\n")
string(APPEND text "namespace {\n\n")
string(APPEND text "const unsigned char kBytes[${size}] = {\n${bytes}\n};\n\n")
string(APPEND text "}  // namespace\n\n")
string(APPEND text "curia::ui::EmbeddedBytes ${FUNCTION}() {\n")
string(APPEND text "    return {kBytes, sizeof(kBytes)};\n")
string(APPEND text "}\n")

file(WRITE "${OUTPUT}" "${text}")
