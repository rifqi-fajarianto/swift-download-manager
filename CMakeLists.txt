cmake_minimum_required(VERSION 3.28)

# Single source of truth for the application version (see AGENTS.md).
project(SwiftDM
    VERSION 0.21.2
    DESCRIPTION "Clean-room cross-platform download manager (segmented, resumable)"
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
    if(WIN32 AND Qt6_VERSION VERSION_LESS 6.5)
        message(FATAL_ERROR "The Windows UI requires Qt 6.5 or newer")
    endif()
    if(WIN32 AND Qt6_VERSION VERSION_GREATER 6.12)
        message(FATAL_ERROR "Windows 10 support requires Qt 6.12 or older")
    endif()
    if(WIN32)
        find_program(WINDEPLOYQT_EXECUTABLE NAMES windeployqt windeployqt6 REQUIRED)
        find_program(BASH_EXECUTABLE bash REQUIRED)
        set(SWIFTSDM_WINDOWS_DEPLOY_DIR "${CMAKE_CURRENT_BINARY_DIR}/windows-deploy")
    endif()

    # Regenerates ui-assets/app-icon/main.ico, the icon of the Windows
    # executable and installer, after an icon change:
    # cmake --build build --target render-ico
    add_executable(render-ico tools/render_ico.cpp)
    target_link_libraries(render-ico PRIVATE Qt6::Gui)
    target_compile_definitions(render-ico PRIVATE
        SWIFTSDM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

    # Regenerates ext/chromium/icons/*.png from the SVG sources after an icon
    # change: cmake --build build --target render-extension-icons
    add_executable(render-extension-icons tools/render_icons.cpp)
    target_link_libraries(render-extension-icons PRIVATE Qt6::Gui)
    target_compile_definitions(render-extension-icons PRIVATE
        SWIFTSDM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

    if(UNIX)
        # Renders the hicolor launcher icons from the app icon SVG.
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
    endif()

    # Icon assets, shared by the app and the headless icons test.
    set(SWIFTSDM_ASSET_ICONS
        "${CMAKE_SOURCE_DIR}/ui-assets/alert.svg"
        "${CMAKE_SOURCE_DIR}/ui-assets/allert-filled.svg"
        "${CMAKE_SOURCE_DIR}/ui-assets/applications.svg"
        "${CMAKE_SOURCE_DIR}/ui-assets/complete.svg"
        "${CMAKE_SOURCE_DIR}/ui-assets/complete-filled.svg"
        "${CMAKE_SOURCE_DIR}/ui-assets/compressed.svg"
        "${CMAKE_SOURCE_DIR}/ui-assets/delete-file.svg"
        "${CMAKE_SOURCE_DIR}/ui-assets/delete.svg"
        "${CMAKE_SOURCE_DIR}/ui-assets/document.svg"
        "${CMAKE_SOURCE_DIR}/ui-assets/folder.svg"
        "${CMAKE_SOURCE_DIR}/ui-assets/info-filled.svg"
        "${CMAKE_SOURCE_DIR}/ui-assets/link-plus.svg"
        "${CMAKE_SOURCE_DIR}/ui-assets/link.svg"
        "${CMAKE_SOURCE_DIR}/ui-assets/music.svg"
        "${CMAKE_SOURCE_DIR}/ui-assets/other.svg"
        "${CMAKE_SOURCE_DIR}/ui-assets/plus.svg"
        "${CMAKE_SOURCE_DIR}/ui-assets/properties.svg"
        "${CMAKE_SOURCE_DIR}/ui-assets/question-filled.svg"
        "${CMAKE_SOURCE_DIR}/ui-assets/queue-add.svg"
        "${CMAKE_SOURCE_DIR}/ui-assets/queue-delete.svg"
        "${CMAKE_SOURCE_DIR}/ui-assets/queue-settings.svg"
        "${CMAKE_SOURCE_DIR}/ui-assets/queue.svg"
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
    src/core/file_io.cpp
    src/core/util.cpp
    src/engine/categories.cpp
    src/engine/download_engine.cpp
    src/engine/hls.cpp
    src/engine/segment_plan.cpp
    src/net/http.cpp
    src/net/http_request.cpp
    src/net/http_error.cpp
    src/net/http_probe.cpp
    src/net/http_segment.cpp
    src/net/http_stream.cpp
    src/net/interstitial.cpp
    src/store/history.cpp
    src/store/settings.cpp
)
target_include_directories(swiftdm_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
)
target_link_libraries(swiftdm_core PUBLIC CURL::libcurl Threads::Threads SQLite::SQLite3)
if(WIN32)
    # advapi32: registry autostart. ws2_32: sockets used by the CLI and tests.
    target_link_libraries(swiftdm_core PUBLIC advapi32 ws2_32)
endif()

# ---------------------------------------------------------------------------
# CLI / application entry point
# ---------------------------------------------------------------------------
add_executable(swiftdm
    src/app/main.cpp
)
target_link_libraries(swiftdm PRIVATE swiftdm_core)

if(SWIFTSDM_BUILD_UI)
    set_target_properties(swiftdm PROPERTIES AUTOMOC ON)

    # The GUI build uses the Windows GUI subsystem so launching it from Explorer,
    # the installer shortcut or the login entry never opens a console window.
    # CLI runs borrow the parent console at runtime (see main.cpp); the headless
    # build keeps the console subsystem so its output works without one.
    if(WIN32)
        set_target_properties(swiftdm PROPERTIES WIN32_EXECUTABLE ON)

        # The icon lives in the executable's resources; the resource script is
        # generated so the .ico path is absolute whatever working directory the
        # resource compiler is started from.
        enable_language(RC)
        configure_file(src/app/swiftdm.rc.in
                       "${CMAKE_CURRENT_BINARY_DIR}/generated/swiftdm.rc" @ONLY)
        target_sources(swiftdm PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/swiftdm.rc")
    endif()

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
        src/ui/queue_picker_dialog.cpp
        src/ui/properties_dialog.cpp
        src/ui/settings_dialog.cpp
        src/ui/speed_limit_editor.cpp
        src/ui/download_window.cpp
        src/ui/queue_logic.cpp
        src/ui/queue_dialog.cpp
        src/ui/browser_integration.cpp
        src/ui/main_window.cpp
        src/ui/main_window_history.cpp
        src/ui/main_window_downloads.cpp
        src/ui/main_window_rows.cpp
        src/ui/main_window_dialogs.cpp
        src/ui/main_window_queue.cpp
        src/ui/main_window_shutdown.cpp
    )
    target_link_libraries(swiftdm PRIVATE Qt6::Network Qt6::Widgets)
    target_compile_definitions(swiftdm PRIVATE SWIFTSDM_WITH_GUI)

    if(WIN32)
        # Produce a runnable directory with the Qt runtime and SVG plugin, not
        # just an executable linked against build-machine DLLs. The dependency
        # copy runs against the build-tree executable so the loader resolves the
        # runtime from the toolchain instead of the already-deployed copies.
        add_custom_command(TARGET swiftdm POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${SWIFTSDM_WINDOWS_DEPLOY_DIR}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "$<TARGET_FILE:swiftdm>" "${SWIFTSDM_WINDOWS_DEPLOY_DIR}"
            COMMAND "${WINDEPLOYQT_EXECUTABLE}" --release --no-translations
                    --include-plugins qsvg,qsvgicon
                    --dir "${SWIFTSDM_WINDOWS_DEPLOY_DIR}"
                    "${SWIFTSDM_WINDOWS_DEPLOY_DIR}/$<TARGET_FILE_NAME:swiftdm>"
            COMMAND "${BASH_EXECUTABLE}"
                    "${CMAKE_SOURCE_DIR}/tools/deploy_msys_runtime.sh"
                    "$<TARGET_FILE:swiftdm>"
                    "${SWIFTSDM_WINDOWS_DEPLOY_DIR}"
            COMMENT "Deploying the Windows executable and Qt runtime"
            VERBATIM
        )

        # Inno Setup is optional: without it the build and the ZIP package still
        # work, only the windows-installer target is missing.
        find_program(ISCC_EXECUTABLE NAMES ISCC
            HINTS "C:/Program Files (x86)/Inno Setup 6" "C:/Program Files/Inno Setup 6")
        if(ISCC_EXECUTABLE)
            add_custom_target(windows-installer
                COMMAND "${ISCC_EXECUTABLE}" /Qp
                        "/DAppVersion=${PROJECT_VERSION}"
                        "/DSourceDir=${SWIFTSDM_WINDOWS_DEPLOY_DIR}"
                        "/O${CMAKE_CURRENT_BINARY_DIR}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/swiftdm.iss"
                DEPENDS swiftdm
                COMMENT "Building the Windows installer"
                VERBATIM
            )
        endif()
    endif()

    # The launcher icons are rendered at build time from the app icon SVG.
    if(UNIX)
        install(DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/icons/hicolor"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons"
        )
    endif()
endif()

# ---------------------------------------------------------------------------
# Install rules (system install and CPack .deb)
# ---------------------------------------------------------------------------

if(WIN32)
    install(DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/windows-deploy/"
        DESTINATION "."
    )
    # The browser integration ships next to the executable and is loaded as an
    # unpacked extension, like the development copy installed on Unix.
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/ext/chromium/"
        DESTINATION "chromium-extension"
    )
    install(FILES LICENSE README.md
        DESTINATION "."
    )
else()
    install(TARGETS swiftdm
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    )
endif()

if(UNIX)
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
endif()

set(CPACK_PACKAGE_NAME "swiftdm")
set(CPACK_PACKAGE_VENDOR "SwiftDM contributors")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Clean-room download manager (segmented, resumable)")
set(CPACK_PACKAGE_CONTACT "SwiftDM contributors")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "swiftdm")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_STRIP_FILES ON)
if(WIN32)
    set(CPACK_GENERATOR "ZIP")
    set(CPACK_PACKAGE_INSTALL_DIRECTORY "SwiftDM")
else()
    set(CPACK_GENERATOR "DEB")
endif()

# Debian package metadata. Runtime library dependencies are resolved by
# dpkg-shlibdeps from the built binary; only packages that are loaded at run
# time (the SVG image plugin behind the icon resources) are listed here. The
# browser extension is optional and works with any Chromium-based browser, so
# the package must not pull a browser or its printer stack as a dependency.
if(UNIX)
    set(CPACK_DEBIAN_PACKAGE_NAME "swiftdm")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "SwiftDM contributors")
    set(CPACK_DEBIAN_PACKAGE_SECTION "net")
    set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "libqt6svg6, qt6-svg-plugins")
    set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/rifqi-fajarianto/swift-download-manager")
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    set(CPACK_DEBIAN_DEBUGINFO_PACKAGE OFF)
    set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian/postinst;${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian/postrm")
endif()

include(CPack)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(SWIFTSDM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
