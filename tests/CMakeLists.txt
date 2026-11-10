# Tests are CLI/backend only — no GUI automation (see AGENTS.md).

add_executable(test_core_version test_version.cpp)
target_link_libraries(test_core_version PRIVATE swiftdm_core)
add_test(NAME core_version COMMAND test_core_version)

add_executable(test_plan test_plan.cpp)
target_link_libraries(test_plan PRIVATE swiftdm_core)
add_test(NAME plan COMMAND test_plan)

add_executable(test_split_policy test_split_policy.cpp)
target_link_libraries(test_split_policy PRIVATE swiftdm_core)
add_test(NAME split_policy COMMAND test_split_policy)

# Receive-rate gate: the achieved rate must track the configured limit instead
# of saturating at chunk_size / sleep_overhead (the "caps at ~5 MB/s" bug).
add_executable(test_rate_limit test_rate_limit.cpp)
target_link_libraries(test_rate_limit PRIVATE swiftdm_core)
add_test(NAME rate_limit COMMAND test_rate_limit)

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

add_executable(test_file_io test_file_io.cpp)
target_link_libraries(test_file_io PRIVATE swiftdm_core)
add_test(NAME file_io COMMAND test_file_io)

# CA bundle resolution: the Windows package ships ca-bundle.crt next to the
# executable because MSYS2's libcurl defaults to a POSIX CA path.
add_executable(test_ca_bundle test_ca_bundle.cpp)
target_link_libraries(test_ca_bundle PRIVATE swiftdm_core)
add_test(NAME ca_bundle COMMAND test_ca_bundle)

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
    # The release ZIP is built by the same helper the workflow runs, so this
    # test guards the archive layout users load with "Load unpacked".
    add_test(NAME extension_package
        COMMAND ${NODE_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_extension_package.js")
endif()

if(SWIFTSDM_BUILD_UI)
    # Headless (offscreen) icon pipeline check; no windows are shown.
    add_executable(test_icons
        test_icons.cpp
        "${CMAKE_SOURCE_DIR}/src/ui/icons.cpp"
        "${CMAKE_SOURCE_DIR}/src/ui/theme.cpp"
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
        "${CMAKE_SOURCE_DIR}/src/ui/queue_picker_dialog.cpp"
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

    # Headless (offscreen) check of the queue picker: the queue entries, their
    # item counts and the deliberate selection rule. Never shown.
    add_executable(test_queue_picker
        test_queue_picker.cpp
        "${CMAKE_SOURCE_DIR}/src/ui/queue_picker_dialog.cpp"
    )
    target_include_directories(test_queue_picker PRIVATE "${CMAKE_SOURCE_DIR}/src")
    set_target_properties(test_queue_picker PROPERTIES AUTOMOC ON)
    target_link_libraries(test_queue_picker PRIVATE swiftdm_core Qt6::Widgets)
    add_test(NAME queue_picker COMMAND test_queue_picker)
    set_tests_properties(queue_picker PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Headless (offscreen) check of the settings window structure: five tabs
    # and every control on the tab that owns it. The dialog is never shown.
    add_executable(test_settings_dialog
        test_settings_dialog.cpp
        "${CMAKE_SOURCE_DIR}/src/ui/settings_dialog.cpp"
        "${CMAKE_SOURCE_DIR}/src/ui/confirm_download_dialog.cpp"
        "${CMAKE_SOURCE_DIR}/src/ui/queue_picker_dialog.cpp"
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
        "${CMAKE_SOURCE_DIR}/src/ui/confirm_download_dialog.cpp"
        "${CMAKE_SOURCE_DIR}/src/ui/queue_picker_dialog.cpp"
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

    # Headless check of the context-menu dispatch guard: a dismissed menu
    # (nullptr from QMenu::exec) must never match an optional action that the
    # row did not create.
    add_executable(test_menu_action test_menu_action.cpp)
    target_include_directories(test_menu_action PRIVATE "${CMAKE_SOURCE_DIR}/src")
    target_link_libraries(test_menu_action PRIVATE swiftdm_core Qt6::Widgets)
    add_test(NAME menu_action COMMAND test_menu_action)
    set_tests_properties(menu_action PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Headless check of the refresh-download-address routing: a pending refresh
    # never turns a browser capture into a new download, and New URL is blocked.
    add_executable(test_refresh_logic
        test_refresh_logic.cpp
        "${CMAKE_SOURCE_DIR}/src/ui/refresh_logic.cpp"
    )
    target_include_directories(test_refresh_logic PRIVATE "${CMAKE_SOURCE_DIR}/src")
    target_link_libraries(test_refresh_logic PRIVATE swiftdm_core Qt6::Widgets)
    add_test(NAME refresh_logic COMMAND test_refresh_logic)

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

    # Headless check of the single-instance guard: exclusive lock, owner record,
    # stale-lock takeover and the "show the main window" request over the local
    # socket. No window is shown.
    add_executable(test_single_instance
        test_single_instance.cpp
        "${CMAKE_SOURCE_DIR}/src/ui/single_instance.cpp"
    )
    target_include_directories(test_single_instance PRIVATE "${CMAKE_SOURCE_DIR}/src")
    set_target_properties(test_single_instance PROPERTIES AUTOMOC ON)
    target_link_libraries(test_single_instance PRIVATE swiftdm_core Qt6::Network)
    add_test(NAME single_instance COMMAND test_single_instance)

    add_executable(test_browser_protocol
        test_browser_protocol.cpp
        "${CMAKE_SOURCE_DIR}/src/ui/browser_integration.cpp"
    )
    target_include_directories(test_browser_protocol PRIVATE "${CMAKE_SOURCE_DIR}/src")
    set_target_properties(test_browser_protocol PROPERTIES AUTOMOC ON)
    target_link_libraries(test_browser_protocol PRIVATE swiftdm_core Qt6::Network)
    add_test(NAME browser_protocol COMMAND test_browser_protocol)
endif()
