# OFF by default. Actual lifted game code and guarded checkpoint transactions.
# Even when compiled, private practice sessions require explicit launch opt-in.
option(TOOIE_PERSISTENT_RUNTIME_EXPERIMENT "Experimental lifted runtime and guarded persistent checkpoint transactions" OFF)
if(TOOIE_PERSISTENT_RUNTIME_EXPERIMENT)
    if(NOT TOOIE_NATIVE_HOST)
        message(FATAL_ERROR "Persistent runtime experiment requires native host device ownership")
    endif()
    message(WARNING "Experimental persistent runtime: explicit private-practice launch required; normal sessions retain normal progress saving")
    foreach(_property LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
        get_target_property(_persistent_links TooieFoundation ${_property})
        list(REMOVE_ITEM _persistent_links RecompiledFuncs)
        list(APPEND _persistent_links PersistentRecompiledFuncs)
        set_property(TARGET TooieFoundation PROPERTY ${_property} "${_persistent_links}")
    endforeach()
    target_sources(TooieFoundation PRIVATE src/persistent_state_continuation.cpp src/persistent_state_runtime.cpp src/persistent_state_hle.cpp
        src/persistent_state_idle.cpp src/persistent_state_scheduler.cpp src/persistent_state_devices.cpp
        src/persistent_state_file.cpp src/persistent_state_host_bridge.cpp src/persistent_state_controller.cpp
        src/persistent_state_frontend.cpp)
    target_compile_definitions(TooieFoundation PUBLIC TOOIE_PERSISTENT_RUNTIME_EXPERIMENT=1)
    target_compile_definitions(ultramodern PRIVATE TOOIE_PERSISTENT_RUNTIME_EXPERIMENT=1)
    target_compile_definitions(librecomp PRIVATE TOOIE_PERSISTENT_RUNTIME_EXPERIMENT=1)
    add_dependencies(TooieFoundation PersistentRecompiledFuncs)
    set(_persistent_device_tool "${CMAKE_CURRENT_SOURCE_DIR}/tools/prepare_persistent_devices.py")
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${_persistent_device_tool}"
        --input-directory "${CMAKE_BINARY_DIR}/runtime-continuous" --output-directory "${CMAKE_BINARY_DIR}"
        RESULT_VARIABLE _persistent_devices_result ERROR_VARIABLE _persistent_devices_error)
    if(NOT _persistent_devices_result EQUAL 0)
        message(FATAL_ERROR "Persistent device materialization failed: ${_persistent_devices_error}")
    endif()
    add_custom_target(verify_persistent_devices
        COMMAND "${Python3_EXECUTABLE}" "${_persistent_device_tool}"
            --input-directory "${CMAKE_BINARY_DIR}/runtime-continuous" --output-directory "${CMAKE_BINARY_DIR}"
        VERBATIM)
    add_dependencies(verify_persistent_devices verify_runtime_continuous)
    foreach(_target ultramodern librecomp)
        get_target_property(_sources ${_target} SOURCES)
        set(_replaced_sources)
        foreach(_source IN LISTS _sources)
            if(_source MATCHES "/runtime-continuous/(events|timer|pi)\\.cpp$")
                list(APPEND _replaced_sources "${CMAKE_BINARY_DIR}/persistent_${CMAKE_MATCH_1}.cpp")
            else()
                list(APPEND _replaced_sources "${_source}")
            endif()
        endforeach()
        set_property(TARGET ${_target} PROPERTY SOURCES "${_replaced_sources}")
        add_dependencies(${_target} verify_persistent_devices)
    endforeach()
endif()
