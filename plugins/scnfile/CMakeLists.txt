# The scnfile plugin (Monocypher, in monocypher/, decrypts). Built by the engine when this folder is in TVP_PLUGIN_FOLDERS (listed
# after krkrz_dev's plugins, whose tp_stub and ncbind it uses), or on its own through the
# CMakeLists.txt at the top of this repository.
set(TPSTUB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../tp_stub CACHE PATH "Path to tp_stub directory")
include(${TPSTUB_DIR}/krkrz.cmake)

# zlib reads "mdf" (compressed) PSB files
find_package(ZLIB)
set(SCNFILE_LIBRARIES)
if(ZLIB_FOUND)
    list(APPEND SCNFILE_LIBRARIES ZLIB::ZLIB)
endif()

krkrz_plugin(scnfile NCBIND
    SOURCES
        main.cpp
        psb.cpp
        seal.cpp
        monocypher/monocypher.c
    LIBRARIES
        ${SCNFILE_LIBRARIES}
)
target_compile_features(scnfile PRIVATE cxx_std_17)
if(ZLIB_FOUND)
    target_compile_definitions(scnfile PRIVATE SCNFILE_HAVE_ZLIB)
endif()
