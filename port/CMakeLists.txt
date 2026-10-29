# Windows build of the Wii port (same sources as port/wii/Makefile).
#
#   cmake -S port -B build-port -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-port
#   ctest --test-dir build-port --output-on-failure
cmake_minimum_required(VERSION 3.20)
project(idas3_wii_port LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
    add_compile_options(/utf-8)
endif()

# --- libchdr + codecs (BSD-3 / public domain / zlib / BSD) ------------------
set(CHDR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libchdr)
file(GLOB CHDR_SOURCES
    ${CHDR}/src/*.c
    ${CHDR}/deps/lzma-19.00/*.c
    ${CHDR}/deps/zstd-1.5.6/lib/common/*.c
    ${CHDR}/deps/zstd-1.5.6/lib/decompress/*.c)
list(APPEND CHDR_SOURCES
    ${CHDR}/deps/zlib-1.3.1/adler32.c
    ${CHDR}/deps/zlib-1.3.1/crc32.c
    ${CHDR}/deps/zlib-1.3.1/inffast.c
    ${CHDR}/deps/zlib-1.3.1/inflate.c
    ${CHDR}/deps/zlib-1.3.1/inftrees.c
    ${CHDR}/deps/zlib-1.3.1/zutil.c)
add_library(chdr STATIC ${CHDR_SOURCES})
target_include_directories(chdr PUBLIC ${CHDR}/include ${CHDR}/deps/zlib-1.3.1)
target_compile_definitions(chdr PRIVATE _7ZIP_ST ZSTD_DISABLE_ASM)
if(MSVC)
    target_compile_options(chdr PRIVATE /W0)
else()
    target_compile_options(chdr PRIVATE -w)
endif()

# --- shared core ----------------------------------------------------------
set(CORE_SOURCES
    src/core/log.cpp
    src/core/chd_gdrom.cpp
    src/core/naomi_pic.cpp
    src/core/naomi_des.cpp
    src/core/dimm_image.cpp
    src/core/naomi_boot.cpp
    src/core/guest_memory.cpp
    src/core/game_session.cpp
    src/core/bios_image.cpp
    src/platform/platform_win.cpp)
add_library(idas3_core STATIC ${CORE_SOURCES})
target_include_directories(idas3_core PUBLIC src)
target_link_libraries(idas3_core PUBLIC chdr)
if(MSVC)
    target_compile_options(idas3_core PRIVATE /W4 /wd4100)
endif()

# --- forked nullDC4Wii core, NAOMI 2 build (see emu/README.md) -------------
set(EMU ${CMAKE_CURRENT_SOURCE_DIR}/emu)
file(GLOB EMU_SOURCES
    ${EMU}/stdclass.cpp
    ${EMU}/config/*.cpp
    ${EMU}/cl/*.cpp
    ${EMU}/plugins/*.cpp
    ${EMU}/dc/*.cpp
    ${EMU}/dc/sh4/*.cpp
    ${EMU}/dc/sh4/rec_v2/*.cpp
    ${EMU}/dc/mem/*.cpp
    ${EMU}/dc/pvr/*.cpp
    ${EMU}/dc/aica/*.cpp
    ${EMU}/dc/asic/*.cpp
    ${EMU}/dc/maple/*.cpp
    ${EMU}/plugs/drkPvr/*.cpp
    ${EMU}/plugs/nullAICA/*.cpp
    ${EMU}/plugs/vbaARM/*.cpp
    ${EMU}/plugs/nullExtDev/*.cpp
    ${EMU}/naomi/*.cpp
    ${EMU}/host/*.cpp)
# Wii-only: GX renderer, ARM7 self-test, ASND audio stream (arm7_jit.cpp
# compiles to stubs off the Wii). nullAICA's old mem.cpp is superseded by
# aica_mem.cpp (same filter as nullDC4Wii). arm7_cached.cpp needs GCC computed
# goto; MSVC runs the plain arm7.cpp interpreter.
list(FILTER EMU_SOURCES EXCLUDE REGEX "gxRend\\.cpp$|arm7_selftest\\.cpp$|arm7_cached\\.cpp$|wii_audiostream\\.cpp$|nullAICA/mem\\.cpp$")
add_library(nulldc_naomi2 STATIC ${EMU_SOURCES})
target_include_directories(nulldc_naomi2 PUBLIC ${EMU})
target_compile_definitions(nulldc_naomi2 PUBLIC _WINPORT RELEASE DC_PLATFORM=2 _HAS_STD_BYTE=0)
target_link_libraries(nulldc_naomi2 PUBLIC idas3_core)
if(MSVC)
    target_compile_options(nulldc_naomi2 PRIVATE /W1 /bigobj /wd4996 /wd4005
        "/FI${EMU}/host/msvc_compat.h")
    # Stand-ins for the few libogc headers the shared sources include.
    target_include_directories(nulldc_naomi2 PRIVATE ${EMU}/host/include)
endif()

# Headless NAOMI 2 runner (bring-up tool).
add_executable(idas3wii_run src/run/run_main.cpp)
target_link_libraries(idas3wii_run PRIVATE nulldc_naomi2)
target_compile_definitions(idas3wii_run PRIVATE _HAS_STD_BYTE=0)
if(MSVC)
    target_compile_options(idas3wii_run PRIVATE /wd4005 "/FI${EMU}/host/msvc_compat.h")
    target_link_options(idas3wii_run PRIVATE /STACK:8388608)
endif()

# --- the launcher ---------------------------------------------------------
add_executable(idas3wii src/main.cpp src/ui/screen.cpp src/ui/launcher.cpp)
target_link_libraries(idas3wii PRIVATE idas3_core)
if(MSVC)
    target_compile_options(idas3wii PRIVATE /W4 /wd4100)
endif()

# --- tests (no game data needed: they build a synthetic GD-ROM CHD) --------
enable_testing()
add_executable(idas3wii_tests tests/core_tests.cpp tests/fixture.cpp)
target_link_libraries(idas3wii_tests PRIVATE idas3_core)
add_test(NAME core COMMAND idas3wii_tests ${CMAKE_CURRENT_BINARY_DIR}/test_fixture)
