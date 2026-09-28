# Include after Dependencies.cmake. Replace one source, preserving the target settings.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(_threadqueue_patch "${CMAKE_CURRENT_LIST_DIR}/../patches/runtime/threadqueue-removal.patch")
set(_threadqueue_tool "${CMAKE_CURRENT_LIST_DIR}/../tools/prepare_runtime_threadqueue.py")
set(_threadqueue_input "${RUNTIME_ROOT}/ultramodern/src/threadqueue.cpp")
set(_threadqueue_output "${CMAKE_BINARY_DIR}/runtime-threadqueue/threadqueue.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_threadqueue_input}" "${_threadqueue_patch}" "${_threadqueue_tool}")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${_threadqueue_tool}"
    --runtime "${RUNTIME_ROOT}" --patch "${_threadqueue_patch}" --output "${_threadqueue_output}"
    RESULT_VARIABLE _threadqueue_result OUTPUT_VARIABLE _threadqueue_stdout ERROR_VARIABLE _threadqueue_stderr)
if(NOT _threadqueue_result EQUAL 0)
    message(FATAL_ERROR "Mission runtime queue materialization failed: ${_threadqueue_stdout}${_threadqueue_stderr}")
endif()
add_custom_target(verify_runtime_threadqueue
    COMMAND "${Python3_EXECUTABLE}" "${_threadqueue_tool}"
        --runtime "${RUNTIME_ROOT}" --patch "${_threadqueue_patch}" --output "${_threadqueue_output}"
    COMMENT "Verifying pinned runtime queue traversal and original FIFO tie ordering"
    VERBATIM)
add_dependencies(ultramodern verify_runtime_threadqueue)
get_target_property(_threadqueue_sources ultramodern SOURCES)
get_target_property(_threadqueue_source_dir ultramodern SOURCE_DIR)
set(_threadqueue_replaced 0)
set(_threadqueue_new_sources)
foreach(_source IN LISTS _threadqueue_sources)
    get_filename_component(_absolute "${_source}" ABSOLUTE BASE_DIR "${_threadqueue_source_dir}")
    if(_absolute STREQUAL _threadqueue_input)
        math(EXPR _threadqueue_replaced "${_threadqueue_replaced} + 1")
        list(APPEND _threadqueue_new_sources "${_threadqueue_output}")
    else()
        list(APPEND _threadqueue_new_sources "${_source}")
    endif()
endforeach()
if(NOT _threadqueue_replaced EQUAL 1)
    message(FATAL_ERROR "Expected exactly one runtime threadqueue.cpp; found ${_threadqueue_replaced}")
endif()
set_property(TARGET ultramodern PROPERTY SOURCES "${_threadqueue_new_sources}")
file(WRITE "${CMAKE_BINARY_DIR}/runtime-threadqueue/source-substitution.txt"
    "target=ultramodern\nreplaced_count=${_threadqueue_replaced}\noriginal=${_threadqueue_input}\ncompiled=${_threadqueue_output}\n")
