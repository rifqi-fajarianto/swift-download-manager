cmake_minimum_required(VERSION 3.28)

# Single source of truth for the application version (see AGENTS.md).
project(SwiftDM
    VERSION 0.3.0
    DESCRIPTION "Clean-room Linux download manager (segmented, resumable)"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

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
if(SWIFTSDM_BUILD_UI)
    find_package(Qt6 REQUIRED COMPONENTS Widgets)
endif()

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

if(SWIFTSDM_BUILD_UI)
    set_target_properties(swiftdm PROPERTIES AUTOMOC ON)

    # Toolbar/app icons: the four action icons live in ui-assets/ (user-provided
    # Tabler-style SVGs); the remaining glyphs are drawn for this project in
    # src/ui/icons/.
    qt_add_resources(swiftdm "app_icons"
        PREFIX "/icons"
        BASE "ui-assets"
        FILES
            ui-assets/plus.svg
            ui-assets/stop.svg
            ui-assets/resume-start.svg
            ui-assets/delete.svg
    )
    qt_add_resources(swiftdm "ui_icons"
        PREFIX "/icons"
        BASE "src/ui/icons"
        FILES
            src/ui/icons/settings.svg
            src/ui/icons/browsers.svg
            src/ui/icons/about.svg
            src/ui/icons/quit.svg
            src/ui/icons/app.svg
    )

    target_sources(swiftdm PRIVATE
        src/ui/theme.cpp
        src/ui/icons.cpp
        src/ui/download_model.cpp
        src/ui/download_task.cpp
        src/ui/add_download_dialog.cpp
        src/ui/settings_dialog.cpp
        src/ui/download_window.cpp
        src/ui/main_window.cpp
    )
    target_link_libraries(swiftdm PRIVATE Qt6::Widgets)
    target_compile_definitions(swiftdm PRIVATE SWIFTSDM_WITH_GUI)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(SWIFTSDM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
