# scnfile: the plugin and its tests, built on their own (the engine build takes the plugin
# from plugins/scnfile instead). Needs a krkrz_dev checkout for tp_stub and ncbind:
#
#   cmake -B build -DKRKRZ_DEV_DIR=/path/to/krkrz_dev && cmake --build build && ctest --test-dir build
cmake_minimum_required(VERSION 3.16)
project(scnfile_repo C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(SCNFILE_PLUGIN "Build the plugin (needs krkrz_dev)" ON)
option(SCNFILE_SANITIZE "Build the tests with AddressSanitizer and UBSan" OFF)

set(KRKRZ_DEV_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../krkrz_dev CACHE PATH "A krkrz_dev checkout (for tp_stub and ncbind)")
set(TPSTUB_DIR ${KRKRZ_DEV_DIR}/src/plugins/tp_stub CACHE PATH "Path to tp_stub directory")
set(NCBIND_DIR ${KRKRZ_DEV_DIR}/src/plugins/ncbind CACHE PATH "Path to ncbind directory")
set(KRKRZ_VARIANT SDL CACHE STRING "The engine's build variant (SDL, WIN, LIB)")

if(SCNFILE_PLUGIN)
    if(NOT EXISTS ${TPSTUB_DIR}/tp_stub.h)
        message(FATAL_ERROR "No tp_stub at ${TPSTUB_DIR}: set KRKRZ_DEV_DIR to a krkrz_dev checkout, or -DSCNFILE_PLUGIN=OFF")
    endif()
    add_subdirectory(plugins/scnfile)
endif()

# The codec and the encryption, with no engine: scnseal and the tests
enable_testing()
find_package(ZLIB)
add_library(scnfile_core STATIC
    plugins/scnfile/psb.cpp
    plugins/scnfile/seal.cpp
    plugins/scnfile/seal_random.cpp
    plugins/scnfile/monocypher/monocypher.c)
target_include_directories(scnfile_core PUBLIC plugins/scnfile)
if(ZLIB_FOUND)
    target_compile_definitions(scnfile_core PUBLIC SCNFILE_HAVE_ZLIB)
    target_link_libraries(scnfile_core PUBLIC ZLIB::ZLIB)
endif()
if(SCNFILE_SANITIZE)
    target_compile_options(scnfile_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(scnfile_core PUBLIC -fsanitize=address,undefined)
endif()

add_executable(scnseal tools/scnseal.cpp)
target_link_libraries(scnseal PRIVATE scnfile_core)

add_executable(psb_test tests/psb_test.cpp)
target_link_libraries(psb_test PRIVATE scnfile_core)
add_test(NAME psb_test COMMAND psb_test)

add_executable(seal_test tests/seal_test.cpp)
target_link_libraries(seal_test PRIVATE scnfile_core)
add_test(NAME seal_test COMMAND seal_test)
