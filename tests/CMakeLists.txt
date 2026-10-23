# Tests are CLI/backend only — no GUI automation (see AGENTS.md).

add_executable(test_core_version test_version.cpp)
target_link_libraries(test_core_version PRIVATE swiftdm_core)
add_test(NAME core_version COMMAND test_core_version)

add_executable(test_plan test_plan.cpp)
target_link_libraries(test_plan PRIVATE swiftdm_core)
add_test(NAME plan COMMAND test_plan)

add_executable(test_settings test_settings.cpp)
target_link_libraries(test_settings PRIVATE swiftdm_core)
add_test(NAME settings COMMAND test_settings)

add_executable(test_history test_history.cpp)
target_link_libraries(test_history PRIVATE swiftdm_core)
add_test(NAME history COMMAND test_history)

add_executable(test_restore test_restore.cpp)
target_link_libraries(test_restore PRIVATE swiftdm_core)
add_test(NAME restore COMMAND test_restore)

add_executable(test_media test_media.cpp)
target_link_libraries(test_media PRIVATE swiftdm_core)
add_test(NAME media COMMAND test_media)

# File name rules: length limit of a path component, Windows device names and
# the temporary workspace of a download with a very long name.
add_executable(test_filenames test_filenames.cpp)
target_link_libraries(test_filenames PRIVATE swiftdm_core)
add_test(NAME filenames COMMAND test_filenames)

add_executable(test_http test_http.cpp)
target_link_libraries(test_http PRIVATE swiftdm_core)
add_test(NAME http COMMAND test_http)

# The extension logic runs headlessly with mocked Chrome APIs and WebSocket.
find_program(NODE_EXECUTABLE node)
if(NODE_EXECUTABLE)
    add_test(NAME extension_logic
        COMMAND ${NODE_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_extension_logic.js")
    add_test(NAME extension_popup
        COMMAND ${NODE_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_extension_popup.js")
endif()

if(SWIFTSDM_BUILD_UI)
    # Headless (offscreen) icon pipeline check; no windows are shown.
    add_executable(test_icons
        test_icons.cpp
        "${CMAKE_SOURCE_DIR}/src/ui/icons.cpp"
    )
    target_link_libraries(test_icons PRIVATE swiftdm_core Qt6::Widgets)
    target_compile_definitions(test_icons PRIVATE
        SWIFTSDM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    qt_add_resources(test_icons "test_icons_app"
        PREFIX "/icons"
        BASE "${CMAKE_SOURCE_DIR}/ui-assets"
        FILES ${SWIFTSDM_ASSET_ICONS}
    )
    qt_add_resources(test_icons "test_icons_ui"
        PREFIX "/icons"
        BASE "${CMAKE_SOURCE_DIR}/src/ui/icons"
        FILES ${SWIFTSDM_UI_ICONS}
    )
    add_test(NAME icons COMMAND test_icons)
    set_tests_properties(icons PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Headless (offscreen) check of the confirmation dialog geometry; the
    # dialog is never shown.
    add_executable(test_confirm_dialog
        test_confirm_dialog.cpp
        "${CMAKE_SOURCE_DIR}/src/ui/confirm_download_dialog.cpp"
        "${CMAKE_SOURCE_DIR}/src/ui/icons.cpp"
    )
    target_include_directories(test_confirm_dialog PRIVATE "${CMAKE_SOURCE_DIR}/src")
    set_target_properties(test_confirm_dialog PROPERTIES AUTOMOC ON)
    target_link_libraries(test_confirm_dialog PRIVATE swiftdm_core Qt6::Widgets)
    qt_add_resources(test_confirm_dialog "test_confirm_dialog_icons"
        PREFIX "/icons"
        BASE "${CMAKE_SOURCE_DIR}/ui-assets"
        FILES ${SWIFTSDM_ASSET_ICONS}
    )
    add_test(NAME confirm_dialog COMMAND test_confirm_dialog)
    set_tests_properties(confirm_dialog PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Headless (offscreen) check of the settings window structure: five tabs
    # and every control on the tab that owns it. The dialog is never shown.
    add_executable(test_settings_dialog
        test_settings_dialog.cpp
        "${CMAKE_SOURCE_DIR}/src/ui/settings_dialog.cpp"
        "${CMAKE_SOURCE_DIR}/src/ui/speed_limit_editor.cpp"
        "${CMAKE_SOURCE_DIR}/src/ui/icons.cpp"
    )
    target_include_directories(test_settings_dialog PRIVATE "${CMAKE_SOURCE_DIR}/src")
    set_target_properties(test_settings_dialog PROPERTIES AUTOMOC ON)
    target_link_libraries(test_settings_dialog PRIVATE swiftdm_core Qt6::Widgets)
    qt_add_resources(test_settings_dialog "test_settings_dialog_icons"
        PREFIX "/icons"
        BASE "${CMAKE_SOURCE_DIR}/ui-assets"
        FILES ${SWIFTSDM_ASSET_ICONS}
    )
    add_test(NAME settings_dialog COMMAND test_settings_dialog)
    set_tests_properties(settings_dialog PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Headless (offscreen) check of the Download Properties window: the four
    # labeled sections, their fields, and the conditional source rows. The
    # dialog is never shown.
    add_executable(test_properties_dialog
        test_properties_dialog.cpp
        "${CMAKE_SOURCE_DIR}/src/ui/properties_dialog.cpp"
        "${CMAKE_SOURCE_DIR}/src/ui/download_model.cpp"
        "${CMAKE_SOURCE_DIR}/src/ui/download_task.cpp"
        "${CMAKE_SOURCE_DIR}/src/ui/icons.cpp"
    )
    target_include_directories(test_properties_dialog PRIVATE "${CMAKE_SOURCE_DIR}/src")
    set_target_properties(test_properties_dialog PROPERTIES AUTOMOC ON)
    target_link_libraries(test_properties_dialog PRIVATE swiftdm_core Qt6::Widgets)
    qt_add_resources(test_properties_dialog "test_properties_dialog_icons"
        PREFIX "/icons"
        BASE "${CMAKE_SOURCE_DIR}/ui-assets"
        FILES ${SWIFTSDM_ASSET_ICONS}
    )
    qt_add_resources(test_properties_dialog "test_properties_dialog_ui_icons"
        PREFIX "/icons"
        BASE "${CMAKE_SOURCE_DIR}/src/ui/icons"
        FILES ${SWIFTSDM_UI_ICONS}
    )
    add_test(NAME properties_dialog COMMAND test_properties_dialog)
    set_tests_properties(properties_dialog PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Headless (offscreen) check of the Download window: the three tabs, the
    # labeled sections on each tab, and the fields they own. The window is
    # never shown.
    add_executable(test_download_window
        test_download_window.cpp
        "${CMAKE_SOURCE_DIR}/src/ui/download_window.cpp"
        "${CMAKE_SOURCE_DIR}/src/ui/download_model.cpp"
        "${CMAKE_SOURCE_DIR}/src/ui/download_task.cpp"
        "${CMAKE_SOURCE_DIR}/src/ui/speed_limit_editor.cpp"
        "${CMAKE_SOURCE_DIR}/src/ui/icons.cpp"
    )
    target_include_directories(test_download_window PRIVATE "${CMAKE_SOURCE_DIR}/src")
    set_target_properties(test_download_window PROPERTIES AUTOMOC ON)
    target_link_libraries(test_download_window PRIVATE swiftdm_core Qt6::Widgets)
    qt_add_resources(test_download_window "test_download_window_icons"
        PREFIX "/icons"
        BASE "${CMAKE_SOURCE_DIR}/ui-assets"
        FILES ${SWIFTSDM_ASSET_ICONS}
    )
    qt_add_resources(test_download_window "test_download_window_ui_icons"
        PREFIX "/icons"
        BASE "${CMAKE_SOURCE_DIR}/src/ui/icons"
        FILES ${SWIFTSDM_UI_ICONS}
    )
    add_test(NAME download_window COMMAND test_download_window)
    set_tests_properties(download_window PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Queue scheduling rules: insertion order, limit pauses, speed sharing,
    # all-complete detection and the shutdown countdown. The test only uses the
    # pure logic module, so no window is created.
    add_executable(test_queue_logic
        test_queue_logic.cpp
        "${CMAKE_SOURCE_DIR}/src/ui/queue_logic.cpp"
    )
    target_include_directories(test_queue_logic PRIVATE "${CMAKE_SOURCE_DIR}/src")
    target_link_libraries(test_queue_logic PRIVATE swiftdm_core Qt6::Widgets)
    add_test(NAME queue_logic COMMAND test_queue_logic)

    # Headless (offscreen) check of the Queue Properties dialog: sections,
    # defaults, special values and the result round-trip. Never shown.
    add_executable(test_queue_dialog
        test_queue_dialog.cpp
        "${CMAKE_SOURCE_DIR}/src/ui/queue_dialog.cpp"
        "${CMAKE_SOURCE_DIR}/src/ui/speed_limit_editor.cpp"
    )
    target_include_directories(test_queue_dialog PRIVATE "${CMAKE_SOURCE_DIR}/src")
    set_target_properties(test_queue_dialog PROPERTIES AUTOMOC ON)
    target_link_libraries(test_queue_dialog PRIVATE swiftdm_core Qt6::Widgets)
    add_test(NAME queue_dialog COMMAND test_queue_dialog)
    set_tests_properties(queue_dialog PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Headless check of the toolbar Resume/Stop availability rules.
    add_executable(test_action_state test_action_state.cpp)
    target_include_directories(test_action_state PRIVATE "${CMAKE_SOURCE_DIR}/src")
    target_link_libraries(test_action_state PRIVATE swiftdm_core Qt6::Widgets)
    add_test(NAME action_state COMMAND test_action_state)

    # Headless (offscreen) check of the theme module: the setting value, the
    # two palettes and the "follow system" resolution. No window is shown.
    add_executable(test_theme
        test_theme.cpp
        "${CMAKE_SOURCE_DIR}/src/ui/theme.cpp"
    )
    target_include_directories(test_theme PRIVATE "${CMAKE_SOURCE_DIR}/src")
    set_target_properties(test_theme PROPERTIES AUTOMOC ON)
    target_link_libraries(test_theme PRIVATE swiftdm_core Qt6::Widgets)
    add_test(NAME theme COMMAND test_theme)
    set_tests_properties(theme PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(test_browser_protocol
        test_browser_protocol.cpp
        "${CMAKE_SOURCE_DIR}/src/ui/browser_integration.cpp"
    )
    target_include_directories(test_browser_protocol PRIVATE "${CMAKE_SOURCE_DIR}/src")
    set_target_properties(test_browser_protocol PROPERTIES AUTOMOC ON)
    target_link_libraries(test_browser_protocol PRIVATE swiftdm_core Qt6::Network)
    add_test(NAME browser_protocol COMMAND test_browser_protocol)
endif()
