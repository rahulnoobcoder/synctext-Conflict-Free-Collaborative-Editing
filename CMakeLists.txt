cmake_minimum_required(VERSION 3.16)
project(SyncText LANGUAGES CXX)

# ============================================================
# Global settings
# ============================================================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Put binaries in the project root so they behave like the Makefile build.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}")

# ============================================================
# Platform-specific dependencies
# ============================================================
if(WIN32)
  # -----------------------------------------------------------
  # Winsock2  (ws2_32.lib is already #pragma comment'd in
  # socket_compat.h, but we also add it here for MSVC/MinGW.)
  # -----------------------------------------------------------
  set(PLATFORM_LIBS ws2_32)

  # -----------------------------------------------------------
  # PDCurses — same ncurses-compatible API, available on Windows.
  #
  # Search order:
  #   1. CMake package config  (vcpkg: find_package(pdcurses CONFIG))
  #   2. Manual PDCURSES_ROOT  (set via -DPDCURSES_ROOT=<path> or env var)
  #
  # If neither is found, CMake errors out with a helpful message.
  # -----------------------------------------------------------

  # Try vcpkg / package-manager install first.
  find_package(pdcurses CONFIG QUIET)

  if(pdcurses_FOUND)
    message(STATUS "PDCurses found via CMake package config.")
    list(APPEND PLATFORM_LIBS pdcurses)
    # vcpkg sets up include paths automatically.
  else()
    # Fall back to PDCURSES_ROOT for manual installs.
    if(NOT PDCURSES_ROOT)
      # Check environment variable as a convenience.
      if(DEFINED ENV{PDCURSES_ROOT})
        set(PDCURSES_ROOT "$ENV{PDCURSES_ROOT}" CACHE PATH "PDCurses install root")
      endif()
    endif()

    if(PDCURSES_ROOT)
      message(STATUS "PDCurses: using manual path PDCURSES_ROOT=${PDCURSES_ROOT}")
      set(PDCURSES_INCLUDE_DIR "${PDCURSES_ROOT}/include"
          CACHE PATH "PDCurses include directory")
      set(PDCURSES_LIBRARY
          "${PDCURSES_ROOT}/lib/pdcurses.lib"
          CACHE FILEPATH "PDCurses library")

      # MinGW uses .a extension; fall back automatically.
      if(NOT EXISTS "${PDCURSES_LIBRARY}")
        set(PDCURSES_LIBRARY "${PDCURSES_ROOT}/lib/libpdcurses.a"
            CACHE FILEPATH "PDCurses library" FORCE)
      endif()

      if(NOT EXISTS "${PDCURSES_LIBRARY}")
        message(FATAL_ERROR
          "PDCurses library not found under PDCURSES_ROOT=${PDCURSES_ROOT}.\n"
          "Expected one of:\n"
          "  ${PDCURSES_ROOT}/lib/pdcurses.lib   (MSVC)\n"
          "  ${PDCURSES_ROOT}/lib/libpdcurses.a  (MinGW)\n"
          "Install PDCurses (https://pdcurses.org or via vcpkg) and re-run CMake.")
      endif()

      list(APPEND PLATFORM_INCLUDE_DIRS "${PDCURSES_INCLUDE_DIR}")
      list(APPEND PLATFORM_LIBS "${PDCURSES_LIBRARY}")
    else()
      message(FATAL_ERROR
        "PDCurses not found.\n"
        "Install it using one of:\n"
        "  vcpkg install pdcurses            (then re-run CMake with the vcpkg toolchain)\n"
        "  -DPDCURSES_ROOT=<path>            (manual install — see README.md)\n"
        "See the Windows build instructions in README.md for details.")
    endif()
  endif()

else()  # POSIX (Linux / macOS)
  # -----------------------------------------------------------
  # pthreads — needed because std::thread uses it on Linux.
  # -----------------------------------------------------------
  find_package(Threads REQUIRED)
  set(PLATFORM_LIBS Threads::Threads)

  # -----------------------------------------------------------
  # ncurses
  # -----------------------------------------------------------
  find_package(Curses REQUIRED)
  list(APPEND PLATFORM_INCLUDE_DIRS "${CURSES_INCLUDE_DIR}")
  list(APPEND PLATFORM_LIBS "${CURSES_LIBRARIES}")
endif()

# ============================================================
# Main executable: synctext (editor)
# ============================================================
set(SYNCTEXT_SOURCES
  src/main.cpp
  src/editor.cpp
  src/network.cpp
  src/crdt.cpp
  src/ui.cpp
  src/thread_manager.cpp
  src/utils.cpp
)

add_executable(editor ${SYNCTEXT_SOURCES})

target_include_directories(editor PRIVATE
  "${CMAKE_SOURCE_DIR}/src"
  ${PLATFORM_INCLUDE_DIRS}
)

target_link_libraries(editor PRIVATE ${PLATFORM_LIBS})

# Strict warning flags — mirrors the Makefile.
if(MSVC)
  target_compile_options(editor PRIVATE /W3 /WX-)
else()
  target_compile_options(editor PRIVATE -Wall -Wextra)
endif()

# ============================================================
# Test executable: crdt_test
#
# This target intentionally links ONLY crdt.cpp + utils.cpp —
# no networking, no UI, no platform socket headers.  It must
# compile and pass on all platforms without changes.
# ============================================================
enable_testing()

add_executable(crdt_test
  tests/crdt_test.cpp
  src/crdt.cpp
  src/utils.cpp
)

target_include_directories(crdt_test PRIVATE
  "${CMAKE_SOURCE_DIR}/src"
)

# crdt_test uses only standard C++ — no extra platform libs needed.
if(NOT WIN32)
  target_link_libraries(crdt_test PRIVATE Threads::Threads)
endif()

if(MSVC)
  target_compile_options(crdt_test PRIVATE /W3 /WX-)
else()
  target_compile_options(crdt_test PRIVATE -Wall -Wextra)
endif()

add_test(NAME crdt_test COMMAND crdt_test)

# ============================================================
# Summary
# ============================================================
message(STATUS "")
message(STATUS "=== SyncText CMake Configuration ===")
message(STATUS "  Platform           : ${CMAKE_SYSTEM_NAME}")
message(STATUS "  Compiler           : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  C++ Standard       : ${CMAKE_CXX_STANDARD}")
message(STATUS "  Build type         : ${CMAKE_BUILD_TYPE}")
message(STATUS "  Output directory   : ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
if(WIN32)
  message(STATUS "  Curses backend     : PDCurses")
else()
  message(STATUS "  Curses backend     : ncurses")
endif()
message(STATUS "=====================================")
message(STATUS "")
