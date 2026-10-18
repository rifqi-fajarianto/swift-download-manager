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
