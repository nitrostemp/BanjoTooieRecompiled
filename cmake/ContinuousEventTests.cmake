# Parent-owned CMakeLists.txt integration. Materializer is already included by
# RuntimeContinuousPatch.cmake; rerun configure before compiling the test/app.
add_executable(continuous_events_failure_test tests/continuous_events_failure_test.cpp)
target_compile_definitions(continuous_events_failure_test PRIVATE
    TOOIE_EVENTS_RUNTIME_SOURCE="${CMAKE_BINARY_DIR}/runtime-continuous/events.cpp")
target_include_directories(continuous_events_failure_test PRIVATE
    "${RUNTIME_ROOT}/thirdparty" "${RUNTIME_ROOT}/thirdparty/concurrentqueue")
target_link_libraries(continuous_events_failure_test PRIVATE TooieFoundation ${CMAKE_DL_LIBS})
add_dependencies(continuous_events_failure_test verify_runtime_continuous)
set(_event_failure_modes rsp-false rsp-throw rsp-success gfx-throw gfx-success
    create-throw create-null invalid gfx-init-throw rsp-init-throw shutdown-throw
    init-create-throw init-invalid vi-throw producer-order disabled-gfx-success
    disabled-rsp-success disabled-rsp-false)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND _event_failure_modes init-partial)
endif()
foreach(mode IN LISTS _event_failure_modes)
    add_test(NAME continuous_events_${mode} COMMAND continuous_events_failure_test ${mode})
    set_tests_properties(continuous_events_${mode} PROPERTIES TIMEOUT 10)
endforeach()
set_tests_properties(continuous_events_disabled-rsp-false PROPERTIES WILL_FAIL TRUE)

add_executable(continuous_vi_shutdown_test tests/continuous_vi_shutdown_test.cpp)
target_compile_definitions(continuous_vi_shutdown_test PRIVATE
    TOOIE_EVENTS_RUNTIME_SOURCE="${CMAKE_BINARY_DIR}/runtime-continuous/events.cpp")
target_include_directories(continuous_vi_shutdown_test PRIVATE
    "${RUNTIME_ROOT}/thirdparty" "${RUNTIME_ROOT}/thirdparty/concurrentqueue")
target_link_libraries(continuous_vi_shutdown_test PRIVATE TooieFoundation ${CMAKE_DL_LIBS})
add_dependencies(continuous_vi_shutdown_test verify_runtime_continuous)
foreach(mode stop backwards cadence rollback disabled-backwards disabled-cadence)
    add_test(NAME continuous_vi_${mode} COMMAND continuous_vi_shutdown_test ${mode})
    set_tests_properties(continuous_vi_${mode} PROPERTIES TIMEOUT 10)
endforeach()
