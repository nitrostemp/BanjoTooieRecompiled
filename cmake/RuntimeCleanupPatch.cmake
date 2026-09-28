# Include after Dependencies.cmake. Preserve the existing target and every setting.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(_cleanup_patch "${CMAKE_CURRENT_LIST_DIR}/../patches/runtime/producer-quiescent-cleanup.patch")
set(_cleanup_tool "${CMAKE_CURRENT_LIST_DIR}/../tools/prepare_runtime_cleanup.py")
set(_cleanup_input "${RUNTIME_ROOT}/ultramodern/src/threads.cpp")
set(_cleanup_output "${CMAKE_BINARY_DIR}/runtime-cleanup/threads.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_cleanup_input}" "${_cleanup_patch}" "${_cleanup_tool}")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${_cleanup_tool}"
    --runtime "${RUNTIME_ROOT}" --patch "${_cleanup_patch}" --output "${_cleanup_output}"
    RESULT_VARIABLE _cleanup_result OUTPUT_VARIABLE _cleanup_stdout ERROR_VARIABLE _cleanup_stderr)
if(NOT _cleanup_result EQUAL 0)
    message(FATAL_ERROR "Approved runtime cleanup materialization failed: ${_cleanup_stdout}${_cleanup_stderr}")
endif()
# Recheck at build time as well: a dependency revision can change without a
# threads.cpp timestamp change. The generator preserves unchanged output bytes.
add_custom_target(verify_runtime_cleanup
    COMMAND "${Python3_EXECUTABLE}" "${_cleanup_tool}"
        --runtime "${RUNTIME_ROOT}" --patch "${_cleanup_patch}" --output "${_cleanup_output}"
    COMMENT "Verifying pinned runtime and exact approved cleanup overlay"
    VERBATIM)
add_dependencies(ultramodern verify_runtime_cleanup)
get_target_property(_cleanup_sources ultramodern SOURCES)
get_target_property(_cleanup_source_dir ultramodern SOURCE_DIR)
set(_cleanup_replaced 0)
set(_cleanup_new_sources)
foreach(_source IN LISTS _cleanup_sources)
    get_filename_component(_absolute "${_source}" ABSOLUTE BASE_DIR "${_cleanup_source_dir}")
    if(_absolute STREQUAL _cleanup_input)
        math(EXPR _cleanup_replaced "${_cleanup_replaced} + 1")
        list(APPEND _cleanup_new_sources "${_cleanup_output}")
    else()
        list(APPEND _cleanup_new_sources "${_source}")
    endif()
endforeach()
if(NOT _cleanup_replaced EQUAL 1)
    message(FATAL_ERROR "Expected exactly one runtime threads.cpp; found ${_cleanup_replaced}")
endif()
set_property(TARGET ultramodern PROPERTY SOURCES "${_cleanup_new_sources}")
file(WRITE "${CMAKE_BINARY_DIR}/runtime-cleanup/source-substitution.txt"
    "target=ultramodern\nreplaced_count=${_cleanup_replaced}\noriginal=${_cleanup_input}\ncompiled=${_cleanup_output}\n")
