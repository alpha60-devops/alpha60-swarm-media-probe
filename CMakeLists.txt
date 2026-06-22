cmake_minimum_required(VERSION 3.20)
project(MediaEnrichment VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -O2 -pthread")

# Find rapidjson
find_path(RAPIDJSON_INCLUDE_DIR
    NAMES rapidjson/document.h
    PATHS /usr/include /usr/local/include
    REQUIRED
)

# Find mediainfo binary
find_program(MEDIAINFO_EXECUTABLE mediainfo REQUIRED)

# Find OpenSSL
find_package(OpenSSL REQUIRED)

# Find Boost (required by libtorrent)
find_package(Boost REQUIRED COMPONENTS system filesystem thread)

# Find libtorrent (system)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBTORRENT REQUIRED libtorrent-rasterbar)

# Include directories
include_directories(${RAPIDJSON_INCLUDE_DIR})
include_directories(${LIBTORRENT_INCLUDE_DIRS})
include_directories(${Boost_INCLUDE_DIRS})
include_directories(${OPENSSL_INCLUDE_DIR})

# Source files
set(SOURCES
    src/main.cpp
    src/torrent_parser.cpp
    src/torrent_downloader.cpp
    src/mediainfo_extractor.cpp
    src/json_enricher.cpp
)

# Executable
add_executable(media_enrichment ${SOURCES})

# Link libraries
target_link_libraries(media_enrichment
    ${LIBTORRENT_LIBRARIES}
    ${Boost_LIBRARIES}
    ${OPENSSL_LIBRARIES}
    dl
    pthread
)

# Add compile definitions for libtorrent 2.0
target_compile_definitions(media_enrichment PRIVATE
    MEDIAINFO_PATH="${MEDIAINFO_EXECUTABLE}"
    BOOST_SYSTEM_NO_DEPRECATED
    TORRENT_USE_OPENSSL=1
    TORRENT_VERSION_MAJOR=2
)

# Print configuration summary
message(STATUS "=== Configuration Summary ===")
message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "libtorrent: ${LIBTORRENT_LIBRARIES}")
message(STATUS "libtorrent version: ${LIBTORRENT_VERSION}")
message(STATUS "rapidjson: ${RAPIDJSON_INCLUDE_DIR}")
message(STATUS "mediainfo: ${MEDIAINFO_EXECUTABLE}")
message(STATUS "OpenSSL: ${OPENSSL_VERSION}")
message(STATUS "Boost: ${Boost_VERSION}")
message(STATUS "==============================")

# Debug
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -O0 -Wfatal-errors")
