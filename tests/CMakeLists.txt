add_executable(test_core test_core.cpp)
target_link_libraries(test_core PRIVATE lazily)
add_test(NAME core COMMAND test_core)

add_executable(test_smoke test_smoke.cpp)
target_link_libraries(test_smoke PRIVATE lazily)
add_test(NAME smoke COMMAND test_smoke)
