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

add_executable(test_http test_http.cpp)
target_link_libraries(test_http PRIVATE swiftdm_core)
add_test(NAME http COMMAND test_http)

if(SWIFTSDM_BUILD_UI)
    # Headless (offscreen) icon pipeline check; no windows are shown.
    add_executable(test_icons
        test_icons.cpp
        "${CMAKE_SOURCE_DIR}/src/ui/icons.cpp"
    )
    target_link_libraries(test_icons PRIVATE swiftdm_core Qt6::Widgets)
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
endif()
