cmake_minimum_required(VERSION 3.21)

project(swbf2 LANGUAGES C CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR
    "The original Xbox code is 32-bit x86 and runs natively, so this project must be "
    "built as a 32-bit (Win32) program. Configure with: -A Win32")
endif()
if(NOT MSVC)
  message(FATAL_ERROR "Only MSVC is supported (inline x86 assembly and SEH are required).")
endif()

include(cmake/CheckRom.cmake)
swbf2_check_rom("${CMAKE_CURRENT_SOURCE_DIR}/rom")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

find_package(SDL2 CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# Runtime DLL: kernel HLE, NV2A emulation, input, and decompiled game code.
# ---------------------------------------------------------------------------
file(GLOB_RECURSE RUNTIME_SOURCES CONFIGURE_DEPENDS
  runtime/*.cpp runtime/*.c runtime/*.h
  game/*.cpp game/*.c game/*.h)

add_library(swbf2rt SHARED ${RUNTIME_SOURCES})
target_include_directories(swbf2rt PRIVATE runtime game .)
target_compile_definitions(swbf2rt PRIVATE
  WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS SDL_MAIN_HANDLED
  SWBF2_DEFAULT_ROM_DIR="${CMAKE_CURRENT_SOURCE_DIR}/rom"
  SWBF2_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(swbf2rt PRIVATE /W3 /EHa /Zi /utf-8 /wd4100 /wd4996)
target_link_options(swbf2rt PRIVATE /DEBUG /SAFESEH:NO /LARGEADDRESSAWARE)
if(TARGET SDL2::SDL2-static)
  target_link_libraries(swbf2rt PRIVATE SDL2::SDL2-static)
else()
  target_link_libraries(swbf2rt PRIVATE SDL2::SDL2)
endif()
target_link_libraries(swbf2rt PRIVATE opengl32 winmm dbghelp shell32)

# ---------------------------------------------------------------------------
# Loader EXE. It has no CRT and no .text section of its own: its image is a
# 64 MiB placeholder starting at 0x00010000, which the runtime overwrites with
# the XBE (the Xbox image base). See loader/loader.c.
# ---------------------------------------------------------------------------
add_executable(swbf2 loader/loader.c)
set_source_files_properties(loader/loader.c PROPERTIES COMPILE_OPTIONS "/GS-;/Od")
target_link_options(swbf2 PRIVATE
  /NODEFAULTLIB /ENTRY:loader_entry /SUBSYSTEM:CONSOLE
  /BASE:0x10000 /FIXED /DYNAMICBASE:NO /LARGEADDRESSAWARE /SAFESEH:NO
  /SECTION:.xbox,ERW /MANIFEST:NO /INCREMENTAL:NO)
target_link_libraries(swbf2 PRIVATE kernel32 user32)
add_dependencies(swbf2 swbf2rt)
set_target_properties(swbf2 swbf2rt PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# Symbol map for crash reports (function names only, generated from Ghidra).
add_custom_command(TARGET swbf2rt POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/symbols/swbf2.sym" "$<TARGET_FILE_DIR:swbf2rt>/swbf2.sym")
