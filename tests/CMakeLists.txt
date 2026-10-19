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

add_executable(test_media test_media.cpp)
target_link_libraries(test_media PRIVATE swiftdm_core)
add_test(NAME media COMMAND test_media)

add_executable(test_http test_http.cpp)
target_link_libraries(test_http PRIVATE swiftdm_core)
add_test(NAME http COMMAND test_http)

# The extension logic runs headlessly with mocked Chrome APIs and WebSocket.
find_program(NODE_EXECUTABLE node)
if(NODE_EXECUTABLE)
    add_test(NAME extension_logic
        COMMAND ${NODE_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/test_extension_logic.js")
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

    add_executable(test_browser_protocol
        test_browser_protocol.cpp
        "${CMAKE_SOURCE_DIR}/src/ui/browser_integration.cpp"
    )
    target_include_directories(test_browser_protocol PRIVATE "${CMAKE_SOURCE_DIR}/src")
    set_target_properties(test_browser_protocol PROPERTIES AUTOMOC ON)
    target_link_libraries(test_browser_protocol PRIVATE swiftdm_core Qt6::Network)
    add_test(NAME browser_protocol COMMAND test_browser_protocol)
endif()
