cmake_minimum_required(VERSION 3.28)

# Single source of truth for the application version (see AGENTS.md).
project(SwiftDM
    VERSION 0.2.0
    DESCRIPTION "Clean-room Linux download manager (segmented, resumable)"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(SWIFTSDM_BUILD_UI    "Build the Qt6 GUI"      ON)
option(SWIFTSDM_BUILD_TESTS "Build the test suite"  ON)

# ---------------------------------------------------------------------------
# Version header (generated; never edited by hand)
# ---------------------------------------------------------------------------
configure_file(
    cmake/version.h.in
    "${CMAKE_CURRENT_BINARY_DIR}/generated/swiftdm/version.h"
    @ONLY
)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)
find_package(CURL REQUIRED)

# ---------------------------------------------------------------------------
# Core library (engine + network + storage)
# ---------------------------------------------------------------------------
add_library(swiftdm_core STATIC
    src/core/log.cpp
    src/core/util.cpp
    src/engine/categories.cpp
    src/engine/download_engine.cpp
    src/engine/segment_plan.cpp
    src/net/http.cpp
    src/store/settings.cpp
)
target_include_directories(swiftdm_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
)
target_link_libraries(swiftdm_core PUBLIC CURL::libcurl Threads::Threads)

# ---------------------------------------------------------------------------
# CLI / application entry point
# ---------------------------------------------------------------------------
add_executable(swiftdm
    src/app/main.cpp
)
target_link_libraries(swiftdm PRIVATE swiftdm_core)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(SWIFTSDM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
