add_executable(test_core test_core.cpp)
target_link_libraries(test_core PRIVATE lazily)
add_test(NAME core COMMAND test_core)

add_executable(test_smoke test_smoke.cpp)
target_link_libraries(test_smoke PRIVATE lazily)
add_test(NAME smoke COMMAND test_smoke)

add_executable(test_statechart test_statechart.cpp)
target_link_libraries(test_statechart PRIVATE lazily)
add_test(NAME statechart COMMAND test_statechart)

add_executable(test_collections test_collections.cpp)
target_link_libraries(test_collections PRIVATE lazily)
add_test(NAME collections COMMAND test_collections)
