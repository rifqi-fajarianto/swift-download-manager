# Tests are CLI/backend only — no GUI automation (see AGENTS.md).

add_executable(test_core_version test_version.cpp)
target_link_libraries(test_core_version PRIVATE swiftdm_core)
add_test(NAME core_version COMMAND test_core_version)
