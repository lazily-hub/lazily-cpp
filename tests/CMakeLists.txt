add_executable(test_smoke test_smoke.cpp)
target_link_libraries(test_smoke PRIVATE lazily)
add_test(NAME smoke COMMAND test_smoke)

add_executable(test_core test_core.cpp)
target_link_libraries(test_core PRIVATE lazily)
add_test(NAME core COMMAND test_core)

add_executable(test_statechart test_statechart.cpp)
target_link_libraries(test_statechart PRIVATE lazily)
add_test(NAME statechart COMMAND test_statechart)

add_executable(test_collections test_collections.cpp)
target_link_libraries(test_collections PRIVATE lazily)
add_test(NAME collections COMMAND test_collections)

add_executable(test_crdt test_crdt.cpp)
target_link_libraries(test_crdt PRIVATE lazily)
add_test(NAME crdt COMMAND test_crdt)

add_executable(test_ipc test_ipc.cpp)
target_link_libraries(test_ipc PRIVATE lazily lazily_ffi)
add_test(NAME ipc COMMAND test_ipc)

add_executable(test_queue test_queue.cpp)
target_link_libraries(test_queue PRIVATE lazily)
add_test(NAME queue COMMAND test_queue)
