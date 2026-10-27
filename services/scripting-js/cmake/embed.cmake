# Embeds a text file as a NUL-terminated C array:
#   cmake -DIN=<file> -DOUT=<header> -DNAME=<symbol> -P embed.cmake
cmake_minimum_required(VERSION 3.21)
file(READ "${IN}" _hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _bytes "${_hex}")
string(REGEX REPLACE "((0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,)(0x..,))" "\\1\n" _bytes "${_bytes}")
file(WRITE "${OUT}.tmp" "// Generated from ${IN}. Do not edit.\n#pragma once\nstatic const unsigned char ${NAME}[] = {\n${_bytes}0x00};\n")
file(COPY_FILE "${OUT}.tmp" "${OUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUT}.tmp")
