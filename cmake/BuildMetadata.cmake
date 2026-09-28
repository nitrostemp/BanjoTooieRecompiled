# Include after TooieFoundation and enable_testing(). Source inputs
# trigger reconfiguration; older runtime-data/<identity> directories are retained.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(_metadata_tool "${CMAKE_CURRENT_LIST_DIR}/../tools/prepare_build_metadata.py")
set(_metadata_inputs
    "${CMAKE_CURRENT_SOURCE_DIR}/config/tooie.us.toml"
    "${CMAKE_CURRENT_SOURCE_DIR}/generated/boot-reference.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/generated/core2-reference.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/generated/overlay-validation.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_metadata_tool}" ${_metadata_inputs})
execute_process(COMMAND "${Python3_EXECUTABLE}" "${_metadata_tool}"
    --source "${CMAKE_CURRENT_SOURCE_DIR}" --build "${CMAKE_BINARY_DIR}"
    RESULT_VARIABLE _metadata_result OUTPUT_VARIABLE _metadata_stdout ERROR_VARIABLE _metadata_stderr)
if(NOT _metadata_result EQUAL 0)
    message(FATAL_ERROR "Immutable metadata bundle failed: ${_metadata_stdout}${_metadata_stderr}")
endif()
add_custom_target(verify_build_metadata
    COMMAND "${Python3_EXECUTABLE}" "${_metadata_tool}"
        --source "${CMAKE_CURRENT_SOURCE_DIR}" --build "${CMAKE_BINARY_DIR}" --verify
    COMMENT "Verifying immutable build metadata identity" VERBATIM)
target_sources(TooieFoundation PRIVATE src/build_metadata.cpp)
target_include_directories(TooieFoundation PRIVATE "${CMAKE_BINARY_DIR}/build-metadata")
add_dependencies(TooieFoundation verify_build_metadata)
set(_metadata_test_flags)
if(MSVC)
    list(APPEND _metadata_test_flags --msvc)
endif()
add_test(NAME build_metadata COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/build_metadata_test.py"
    --app "${CMAKE_CURRENT_SOURCE_DIR}" --compiler "${CMAKE_CXX_COMPILER}"
    --output "${TOOIE_TEST_EVIDENCE_ROOT}/build-metadata" ${_metadata_test_flags})
set_tests_properties(build_metadata PROPERTIES TIMEOUT 120)
