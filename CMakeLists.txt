cmake_minimum_required(VERSION 3.28)

# Single source of truth for the application version (see AGENTS.md).
project(SwiftDM
    VERSION 0.4.0
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

    # Icon assets, shared by the app and the headless icons test.
    set(SWIFTSDM_ASSET_ICONS
        "${CMAKE_SOURCE_DIR}/ui-assets/alert.svg"
        "${CMAKE_SOURCE_DIR}/ui-assets/applications.svg"
        "${CMAKE_SOURCE_DIR}/ui-assets/complete.svg"
        "${CMAKE_SOURCE_DIR}/ui-assets/compressed.svg"
        "${CMAKE_SOURCE_DIR}/ui-assets/delete-file.svg"
        "${CMAKE_SOURCE_DIR}/ui-assets/delete.svg"
        "${CMAKE_SOURCE_DIR}/ui-assets/document.svg"
        "${CMAKE_SOURCE_DIR}/ui-assets/folder.svg"
        "${CMAKE_SOURCE_DIR}/ui-assets/link-plus.svg"
        "${CMAKE_SOURCE_DIR}/ui-assets/link.svg"
        "${CMAKE_SOURCE_DIR}/ui-assets/music.svg"
        "${CMAKE_SOURCE_DIR}/ui-assets/other.svg"
        "${CMAKE_SOURCE_DIR}/ui-assets/plus.svg"
        "${CMAKE_SOURCE_DIR}/ui-assets/properties.svg"
        "${CMAKE_SOURCE_DIR}/ui-assets/resume-start.svg"
        "${CMAKE_SOURCE_DIR}/ui-assets/settings.svg"
        "${CMAKE_SOURCE_DIR}/ui-assets/stop.svg"
        "${CMAKE_SOURCE_DIR}/ui-assets/video.svg"
    )
    set(SWIFTSDM_UI_ICONS
        "${CMAKE_SOURCE_DIR}/src/ui/icons/browsers.svg"
        "${CMAKE_SOURCE_DIR}/src/ui/icons/about.svg"
        "${CMAKE_SOURCE_DIR}/src/ui/icons/quit.svg"
        "${CMAKE_SOURCE_DIR}/src/ui/icons/app.svg"
    )
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

    # Toolbar/category icons: SVGs in ui-assets/ (provided for this project) plus
    # glyphs drawn for this project in src/ui/icons/.
    qt_add_resources(swiftdm "app_icons"
        PREFIX "/icons"
        BASE "${CMAKE_SOURCE_DIR}/ui-assets"
        FILES ${SWIFTSDM_ASSET_ICONS}
    )
    qt_add_resources(swiftdm "ui_icons"
        PREFIX "/icons"
        BASE "${CMAKE_SOURCE_DIR}/src/ui/icons"
        FILES ${SWIFTSDM_UI_ICONS}
    )

    target_sources(swiftdm PRIVATE
        src/ui/theme.cpp
        src/ui/icons.cpp
        src/ui/download_model.cpp
        src/ui/download_task.cpp
        src/ui/add_download_dialog.cpp
        src/ui/confirm_download_dialog.cpp
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
