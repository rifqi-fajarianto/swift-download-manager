cmake_minimum_required(VERSION 3.28)

# Single source of truth for the application version (see AGENTS.md).
project(SwiftDM
    VERSION 0.18.15
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

# The install destinations are needed by the install rules of the UI block, so
# the module is included before them.
include(GNUInstallDirs)

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
find_package(SQLite3 REQUIRED)
if(SWIFTSDM_BUILD_UI)
    find_package(Qt6 REQUIRED COMPONENTS Network Widgets)

    # Regenerates ext/chromium/icons/*.png from the SVG sources after an icon
    # change: cmake --build build --target render-extension-icons
    add_executable(render-extension-icons tools/render_icons.cpp)
    target_link_libraries(render-extension-icons PRIVATE Qt6::Gui)
    target_compile_definitions(render-extension-icons PRIVATE
        SWIFTSDM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

    # Renders the hicolor launcher icons from the app icon SVG. The build runs
    # it automatically, so a changed app icon reaches the installed icons.
    add_executable(render-desktop-icons tools/render_desktop_icons.cpp)
    target_link_libraries(render-desktop-icons PRIVATE Qt6::Gui)
    target_compile_definitions(render-desktop-icons PRIVATE
        SWIFTSDM_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
        SWIFTSDM_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}")

    set(SWIFTSDM_DESKTOP_ICON_SIZES 16 24 32 48 64 128 256 512)
    set(SWIFTSDM_DESKTOP_ICON_FILES "")
    foreach(size IN LISTS SWIFTSDM_DESKTOP_ICON_SIZES)
        list(APPEND SWIFTSDM_DESKTOP_ICON_FILES
            "${CMAKE_CURRENT_BINARY_DIR}/generated/icons/hicolor/${size}x${size}/apps/swiftdm.png")
    endforeach()
    add_custom_command(
        OUTPUT ${SWIFTSDM_DESKTOP_ICON_FILES}
        COMMAND "$<TARGET_FILE:render-desktop-icons>"
                "${CMAKE_SOURCE_DIR}/ui-assets/app-icon/main-icon.svg"
                "${CMAKE_CURRENT_BINARY_DIR}/generated/icons/hicolor"
        DEPENDS "${CMAKE_SOURCE_DIR}/ui-assets/app-icon/main-icon.svg" render-desktop-icons
        COMMENT "Rendering the desktop launcher icons from main-icon.svg"
        VERBATIM
    )
    add_custom_target(swiftdm-desktop-icons ALL DEPENDS ${SWIFTSDM_DESKTOP_ICON_FILES})
    add_dependencies(swiftdm-desktop-icons render-desktop-icons)

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
        "${CMAKE_SOURCE_DIR}/ui-assets/quit.svg"
        "${CMAKE_SOURCE_DIR}/ui-assets/resume-start.svg"
        "${CMAKE_SOURCE_DIR}/ui-assets/settings.svg"
        "${CMAKE_SOURCE_DIR}/ui-assets/stop.svg"
        "${CMAKE_SOURCE_DIR}/ui-assets/video.svg"
        "${CMAKE_SOURCE_DIR}/ui-assets/app-icon/main-icon.svg"
        "${CMAKE_SOURCE_DIR}/ui-assets/app-icon/extension-icon.svg"
        "${CMAKE_SOURCE_DIR}/ui-assets/app-icon/extension-diactivate-icon.svg"
        "${CMAKE_SOURCE_DIR}/ui-assets/app-icon/single-color-icon.svg"
    )
    set(SWIFTSDM_UI_ICONS
        "${CMAKE_SOURCE_DIR}/src/ui/icons/browsers.svg"
        "${CMAKE_SOURCE_DIR}/src/ui/icons/about.svg"
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
    src/engine/hls.cpp
    src/engine/segment_plan.cpp
    src/net/http.cpp
    src/net/interstitial.cpp
    src/store/history.cpp
    src/store/settings.cpp
)
target_include_directories(swiftdm_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
)
target_link_libraries(swiftdm_core PUBLIC CURL::libcurl Threads::Threads SQLite::SQLite3)

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
        src/ui/properties_dialog.cpp
        src/ui/settings_dialog.cpp
        src/ui/speed_limit_editor.cpp
        src/ui/download_window.cpp
        src/ui/browser_integration.cpp
        src/ui/main_window.cpp
    )
    target_link_libraries(swiftdm PRIVATE Qt6::Network Qt6::Widgets)
    target_compile_definitions(swiftdm PRIVATE SWIFTSDM_WITH_GUI)

    # The launcher icons are rendered at build time from the app icon SVG.
    install(DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/icons/hicolor"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/icons"
    )
endif()

# ---------------------------------------------------------------------------
# Install rules (system install and CPack .deb)
# ---------------------------------------------------------------------------

install(TARGETS swiftdm
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

install(FILES packaging/swiftdm.desktop
    DESTINATION "${CMAKE_INSTALL_DATADIR}/applications"
)
install(FILES packaging/io.github.swiftdm.SwiftDM.metainfo.xml
    DESTINATION "${CMAKE_INSTALL_DATADIR}/metainfo"
)
install(FILES LICENSE
    DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/swiftdm"
    RENAME copyright
)
# Development copy of the browser integration; the store-free workflow is to
# load ext/chromium as an unpacked extension from a checkout or from this copy.
install(DIRECTORY ext/chromium
    DESTINATION "${CMAKE_INSTALL_DATADIR}/swiftdm"
)

set(CPACK_PACKAGE_NAME "swiftdm")
set(CPACK_PACKAGE_VENDOR "SwiftDM contributors")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Clean-room Linux download manager (segmented, resumable)")
set(CPACK_PACKAGE_CONTACT "SwiftDM contributors")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "swiftdm")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_STRIP_FILES ON)
set(CPACK_GENERATOR "DEB")

# Debian package metadata. Runtime library dependencies are resolved by
# dpkg-shlibdeps from the built binary; only packages that are loaded at run
# time (the SVG image plugin behind the icon resources) are listed here. The
# browser extension is optional and works with any Chromium-based browser, so
# the package must not pull a browser or its printer stack as a dependency.
set(CPACK_DEBIAN_PACKAGE_NAME "swiftdm")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "SwiftDM contributors")
set(CPACK_DEBIAN_PACKAGE_SECTION "net")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libqt6svg6, qt6-svg-plugins")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/swiftdm/swiftdm")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_DEBUGINFO_PACKAGE OFF)
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian/postinst;${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian/postrm")

include(CPack)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(SWIFTSDM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
