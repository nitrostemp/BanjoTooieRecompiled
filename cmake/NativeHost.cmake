set(TOOIE_RT64_ROOT "${TOOIE_DEPS_ROOT}/rt64" CACHE PATH "Pinned repository-local RT64 checkout")
set(TOOIE_SDL_ROOT "${TOOIE_DEPS_ROOT}/SDL2-2.30.3" CACHE PATH "Pinned Windows SDL development bundle")
foreach(_dependency_path IN ITEMS "${TOOIE_RT64_ROOT}")
    if(NOT EXISTS "${_dependency_path}/CMakeLists.txt")
        message(FATAL_ERROR
            "Pinned native-host dependency is missing at ${_dependency_path}. "
            "Run: python tools/bootstrap_dependencies.py")
    endif()
endforeach()
set(RT64_STATIC ON CACHE BOOL "Static renderer" FORCE)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(RT64_SDL_WINDOW_VULKAN ON CACHE BOOL "SDL Vulkan Linux" FORCE)
else()
    set(RT64_SDL_WINDOW_VULKAN OFF CACHE BOOL "Use the platform RT64 window backend" FORCE)
endif()
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "No compression CLI" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "No upstream tests" FORCE)
add_compile_definitions(HLSL_CPU __PRFCHWINTRIN_H NOMINMAX)
if(WIN32)
    include(cmake/Rt64DxcOverlay.cmake)
    add_subdirectory("${_tooie_rt64_cmake_overlay}" "${CMAKE_BINARY_DIR}/rt64")
else()
    add_subdirectory("${TOOIE_RT64_ROOT}" "${CMAKE_BINARY_DIR}/rt64")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(_rt64_matching_tool "${CMAKE_CURRENT_SOURCE_DIR}/tools/prepare_rt64_matching.py")
set(_rt64_matching_recipe "${CMAKE_CURRENT_SOURCE_DIR}/patches/rt64/game_frame_matching.delta.json")
set(_rt64_matching_input "${TOOIE_RT64_ROOT}/src/hle/rt64_game_frame.cpp")
set(_rt64_matching_overlay "${CMAKE_CURRENT_BINARY_DIR}/tooie_rt64_overlay/rt64_game_frame.cpp")
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${_rt64_matching_tool}"
        --dependency "${TOOIE_RT64_ROOT}" --recipe "${_rt64_matching_recipe}"
        --output "${_rt64_matching_overlay}"
    RESULT_VARIABLE _rt64_matching_result
    OUTPUT_VARIABLE _rt64_matching_stdout ERROR_VARIABLE _rt64_matching_stderr)
if(NOT _rt64_matching_result EQUAL 0)
    message(FATAL_ERROR "Pinned RT64 matching materialization failed: ${_rt64_matching_stdout}${_rt64_matching_stderr}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_rt64_matching_tool}" "${_rt64_matching_recipe}" "${_rt64_matching_input}")
set(_rt64_projection_recipe "${CMAKE_CURRENT_SOURCE_DIR}/patches/rt64/projection_preservation.delta.json")
set(_rt64_projection_input "${TOOIE_RT64_ROOT}/src/render/rt64_projection_processor.cpp")
set(_rt64_projection_overlay "${CMAKE_CURRENT_BINARY_DIR}/tooie_rt64_overlay/rt64_projection_processor.cpp")
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${_rt64_matching_tool}"
        --dependency "${TOOIE_RT64_ROOT}" --recipe "${_rt64_projection_recipe}"
        --output "${_rt64_projection_overlay}"
    RESULT_VARIABLE _rt64_projection_result
    OUTPUT_VARIABLE _rt64_projection_stdout ERROR_VARIABLE _rt64_projection_stderr)
if(NOT _rt64_projection_result EQUAL 0)
    message(FATAL_ERROR "Pinned RT64 projection materialization failed: ${_rt64_projection_stdout}${_rt64_projection_stderr}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_rt64_projection_recipe}" "${_rt64_projection_input}")
set(_rt64_rotation_recipe "${CMAKE_CURRENT_SOURCE_DIR}/patches/rt64/rotation_interpolation.delta.json")
set(_rt64_rotation_input "${TOOIE_RT64_ROOT}/src/common/rt64_math.cpp")
set(_rt64_rotation_overlay "${CMAKE_CURRENT_BINARY_DIR}/tooie_rt64_overlay/rt64_math.cpp")
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${_rt64_matching_tool}"
        --dependency "${TOOIE_RT64_ROOT}" --recipe "${_rt64_rotation_recipe}"
        --output "${_rt64_rotation_overlay}"
    RESULT_VARIABLE _rt64_rotation_result
    OUTPUT_VARIABLE _rt64_rotation_stdout ERROR_VARIABLE _rt64_rotation_stderr)
if(NOT _rt64_rotation_result EQUAL 0)
    message(FATAL_ERROR "Pinned RT64 rotation materialization failed: ${_rt64_rotation_stdout}${_rt64_rotation_stderr}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_rt64_rotation_recipe}" "${_rt64_rotation_input}")
set(_rt64_rigid_body_recipe "${CMAKE_CURRENT_SOURCE_DIR}/patches/rt64/rigid_body.delta.json")
set(_rt64_rigid_body_input "${TOOIE_RT64_ROOT}/src/hle/rt64_rigid_body.cpp")
set(_rt64_rigid_body_overlay "${CMAKE_CURRENT_BINARY_DIR}/tooie_rt64_overlay/rt64_rigid_body.cpp")
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${_rt64_matching_tool}"
        --dependency "${TOOIE_RT64_ROOT}" --recipe "${_rt64_rigid_body_recipe}"
        --output "${_rt64_rigid_body_overlay}"
    RESULT_VARIABLE _rt64_rigid_body_result
    OUTPUT_VARIABLE _rt64_rigid_body_stdout ERROR_VARIABLE _rt64_rigid_body_stderr)
if(NOT _rt64_rigid_body_result EQUAL 0)
    message(FATAL_ERROR "Pinned RT64 rigid-body materialization failed: ${_rt64_rigid_body_stdout}${_rt64_rigid_body_stderr}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_rt64_rigid_body_recipe}" "${_rt64_rigid_body_input}")
set(_rt64_transform_recipe "${CMAKE_CURRENT_SOURCE_DIR}/patches/rt64/transform_upload_guard.delta.json")
set(_rt64_transform_input "${TOOIE_RT64_ROOT}/src/render/rt64_transform_processor.cpp")
set(_rt64_transform_overlay "${CMAKE_CURRENT_BINARY_DIR}/tooie_rt64_overlay/rt64_transform_processor.cpp")
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${_rt64_matching_tool}"
        --dependency "${TOOIE_RT64_ROOT}" --recipe "${_rt64_transform_recipe}"
        --output "${_rt64_transform_overlay}"
    RESULT_VARIABLE _rt64_transform_result
    OUTPUT_VARIABLE _rt64_transform_stdout ERROR_VARIABLE _rt64_transform_stderr)
if(NOT _rt64_transform_result EQUAL 0)
    message(FATAL_ERROR "Pinned RT64 transform materialization failed: ${_rt64_transform_stdout}${_rt64_transform_stderr}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_rt64_transform_recipe}" "${_rt64_transform_input}")
set(_rt64_cpu_pose_recipe "${CMAKE_CURRENT_SOURCE_DIR}/patches/rt64/cpu_model_original_pose.delta.json")
set(_rt64_cpu_pose_input "${TOOIE_RT64_ROOT}/src/hle/rt64_rsp.cpp")
set(_rt64_cpu_pose_overlay "${CMAKE_CURRENT_BINARY_DIR}/tooie_rt64_overlay/rt64_rsp.cpp")
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${_rt64_matching_tool}"
        --dependency "${TOOIE_RT64_ROOT}" --recipe "${_rt64_cpu_pose_recipe}"
        --output "${_rt64_cpu_pose_overlay}"
    RESULT_VARIABLE _rt64_cpu_pose_result
    OUTPUT_VARIABLE _rt64_cpu_pose_stdout ERROR_VARIABLE _rt64_cpu_pose_stderr)
if(NOT _rt64_cpu_pose_result EQUAL 0)
    message(FATAL_ERROR "Pinned RT64 original-pose materialization failed: ${_rt64_cpu_pose_stdout}${_rt64_cpu_pose_stderr}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_rt64_cpu_pose_recipe}" "${_rt64_cpu_pose_input}")
set(_rt64_matrix_trace_recipe "${CMAKE_CURRENT_SOURCE_DIR}/patches/rt64/matrix_trace.delta.json")
set(_rt64_matrix_trace_input "${TOOIE_RT64_ROOT}/src/hle/rt64_workload_queue.cpp")
set(_rt64_matrix_trace_overlay "${CMAKE_CURRENT_BINARY_DIR}/tooie_rt64_overlay/rt64_workload_queue.cpp")
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${_rt64_matching_tool}"
        --dependency "${TOOIE_RT64_ROOT}" --recipe "${_rt64_matrix_trace_recipe}"
        --output "${_rt64_matrix_trace_overlay}"
    RESULT_VARIABLE _rt64_matrix_trace_result
    OUTPUT_VARIABLE _rt64_matrix_trace_stdout ERROR_VARIABLE _rt64_matrix_trace_stderr)
if(NOT _rt64_matrix_trace_result EQUAL 0)
    message(FATAL_ERROR "Pinned RT64 matrix trace materialization failed: ${_rt64_matrix_trace_stdout}${_rt64_matrix_trace_stderr}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_rt64_matrix_trace_recipe}" "${_rt64_matrix_trace_input}")
# Keep the RT64 pin immutable. The second overlay records successful swapchain
# presents into the project's bounded in-memory pacing probe. Each replacement
# below must match exactly one pinned build input.
set(_rt64_console_vi_recipe "${CMAKE_CURRENT_SOURCE_DIR}/patches/rt64/console_vi_writeback.delta.json")
set(_rt64_console_vi_input "${TOOIE_RT64_ROOT}/src/hle/rt64_state.cpp")
set(_rt64_console_vi_overlay "${CMAKE_CURRENT_BINARY_DIR}/tooie_rt64_overlay/rt64_state.cpp")
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${_rt64_matching_tool}"
        --dependency "${TOOIE_RT64_ROOT}" --recipe "${_rt64_console_vi_recipe}"
        --output "${_rt64_console_vi_overlay}"
    RESULT_VARIABLE _rt64_console_vi_result
    OUTPUT_VARIABLE _rt64_console_vi_stdout ERROR_VARIABLE _rt64_console_vi_stderr)
if(NOT _rt64_console_vi_result EQUAL 0)
    message(FATAL_ERROR "Pinned RT64 Console VI materialization failed: ${_rt64_console_vi_stdout}${_rt64_console_vi_stderr}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_rt64_console_vi_recipe}" "${_rt64_console_vi_input}")
set(_rt64_present_input "${TOOIE_RT64_ROOT}/src/hle/rt64_present_queue.cpp")
set(_rt64_present_overlay "${CMAKE_CURRENT_BINARY_DIR}/tooie_rt64_overlay/rt64_present_queue.cpp")
file(READ "${_rt64_present_input}" _rt64_present_source)
set(_rt64_present_include "#include \"rt64_present_queue.h\"")
set(_rt64_present_include_patch "#include \"rt64_present_queue.h\"\n#include \"pacing_present_probe.hpp\"\n#include \"imgui_backend_rt64.hpp\"")
string(FIND "${_rt64_present_source}" "${_rt64_present_include}" _rt64_include_at)
if(_rt64_include_at LESS 0)
    message(FATAL_ERROR "Pinned RT64 present include seam drifted")
endif()
string(REPLACE "${_rt64_present_include}" "${_rt64_present_include_patch}" _rt64_present_source "${_rt64_present_source}")
set(_rt64_present_call "swapChainValid = ext.swapChain->present(swapChainIndex, &waitSemaphore, 1);")
set(_rt64_present_call_patch "ext.swapChain->setVsyncEnabled(tooie_rt64_vsync_requested());\n                swapChainValid = ext.swapChain->present(swapChainIndex, &waitSemaphore, 1);\n                tooie_rt64_present_observed(swapChainValid, ext.swapChain->isVsyncEnabled(), viOriginalRate, targetRate);\n                tooie_rt64_present_trace(present.presentId, present.workloadId, uint32_t(presentationMode), present.screenVI.origin, present.screenVI.fbAddress(), colorTarget ? colorTarget->addressForName : 0, present.screenVI.xTransform.word, present.screenVI.yTransform.word, present.screenVI.hRegion.word, present.screenVI.vRegion.word, i, framesToPresent, viOriginalRate, targetRate, swapChainValid, ext.swapChain->isVsyncEnabled());")
string(FIND "${_rt64_present_source}" "${_rt64_present_call}" _rt64_call_at)
if(_rt64_call_at LESS 0)
    message(FATAL_ERROR "Pinned RT64 present call seam drifted")
endif()
string(REPLACE "${_rt64_present_call}" "${_rt64_present_call_patch}" _rt64_present_source "${_rt64_present_source}")
# RT64's public render hooks lack the swapchain and present worker required by
# its existing ImGui backend. Keep the pin immutable and attach the project UI
# to this small, checked present-queue seam instead.
set(_rt64_imgui_teardown_seam "            delete presentThread;\n        }\n\n        presentIdCondition.notify_all();")
set(_rt64_imgui_setup_seam "        viRenderer = std::make_unique<VIRenderer>();")
set(_rt64_imgui_draw_seam "                    if (inspector != nullptr) {\n                        inspector->draw(commandList);\n                    }")
string(FIND "${_rt64_present_source}" "${_rt64_imgui_teardown_seam}" _rt64_imgui_teardown_at)
string(FIND "${_rt64_present_source}" "${_rt64_imgui_setup_seam}" _rt64_imgui_setup_at)
string(FIND "${_rt64_present_source}" "${_rt64_imgui_draw_seam}" _rt64_imgui_draw_at)
if(_rt64_imgui_teardown_at LESS 0 OR _rt64_imgui_setup_at LESS 0 OR _rt64_imgui_draw_at LESS 0)
    message(FATAL_ERROR "Pinned RT64 ImGui present-queue seam drifted")
endif()
string(REPLACE "            delete presentThread;\n        }\n\n        presentIdCondition.notify_all();"
    "            delete presentThread;\n        }\n\n        tooie::imgui_backend::renderer_shutdown();\n        presentIdCondition.notify_all();"
    _rt64_present_source "${_rt64_present_source}")
string(REPLACE "        viRenderer = std::make_unique<VIRenderer>();"
    "        viRenderer = std::make_unique<VIRenderer>();\n        tooie::imgui_backend::initialize(ext.device, ext.swapChain, ext.createdGraphicsAPI);"
    _rt64_present_source "${_rt64_present_source}")
string(REPLACE "                    if (inspector != nullptr) {\n                        inspector->draw(commandList);\n                    }"
    "                    if (inspector != nullptr) {\n                        inspector->draw(commandList);\n                    }\n                    tooie::imgui_backend::draw(ext.presentGraphicsWorker, commandList);"
    _rt64_present_source "${_rt64_present_source}")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tooie_rt64_overlay")
file(WRITE "${_rt64_present_overlay}" "${_rt64_present_source}")
if(WIN32)
    # The official DXC release signs DXIL inside dxcompiler.dll. The pinned
    # RT64 loader's older mandatory dxil.dll preflight is no longer valid.
    set(_rt64_dynamic_input "${TOOIE_RT64_ROOT}/src/common/rt64_dynamic_libraries.cpp")
    set(_rt64_dynamic_overlay "${CMAKE_CURRENT_BINARY_DIR}/tooie_rt64_overlay/rt64_dynamic_libraries.cpp")
    file(SHA256 "${_rt64_dynamic_input}" _rt64_dynamic_hash)
    if(NOT _rt64_dynamic_hash STREQUAL "d7c56460ef2c9188fa9c1816d71b792b5e25f88e32f2e708fa31bcf5370eaabc")
        message(FATAL_ERROR "Pinned RT64 dynamic library loader changed; review DXC overlay")
    endif()
    file(READ "${_rt64_dynamic_input}" _rt64_dynamic_source)
    string(REPLACE "        NameRequiredPair(\"dxil.dll\", true),\n" ""
        _rt64_dynamic_source "${_rt64_dynamic_source}")
    file(WRITE "${_rt64_dynamic_overlay}" "${_rt64_dynamic_source}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_rt64_dynamic_input}")
endif()
get_target_property(_rt64_sources rt64 SOURCES)
set(_rt64_new_sources)
set(_rt64_present_replaced 0)
set(_rt64_matching_replaced 0)
set(_rt64_projection_replaced 0)
set(_rt64_transform_replaced 0)
set(_rt64_rotation_replaced 0)
set(_rt64_rigid_body_replaced 0)
set(_rt64_matrix_trace_replaced 0)
set(_rt64_cpu_pose_replaced 0)
set(_rt64_console_vi_replaced 0)
set(_rt64_dynamic_replaced 0)
foreach(_source IN LISTS _rt64_sources)
    get_filename_component(_absolute "${_source}" ABSOLUTE BASE_DIR "${TOOIE_RT64_ROOT}")
    if(_absolute STREQUAL _rt64_present_input)
        list(APPEND _rt64_new_sources "${_rt64_present_overlay}")
        math(EXPR _rt64_present_replaced "${_rt64_present_replaced}+1")
    elseif(_absolute STREQUAL _rt64_matching_input)
        list(APPEND _rt64_new_sources "${_rt64_matching_overlay}")
        math(EXPR _rt64_matching_replaced "${_rt64_matching_replaced}+1")
    elseif(_absolute STREQUAL _rt64_projection_input)
        list(APPEND _rt64_new_sources "${_rt64_projection_overlay}")
        math(EXPR _rt64_projection_replaced "${_rt64_projection_replaced}+1")
    elseif(_absolute STREQUAL _rt64_rotation_input)
        list(APPEND _rt64_new_sources "${_rt64_rotation_overlay}")
        math(EXPR _rt64_rotation_replaced "${_rt64_rotation_replaced}+1")
    elseif(_absolute STREQUAL _rt64_rigid_body_input)
        list(APPEND _rt64_new_sources "${_rt64_rigid_body_overlay}")
        math(EXPR _rt64_rigid_body_replaced "${_rt64_rigid_body_replaced}+1")
    elseif(_absolute STREQUAL _rt64_transform_input)
        list(APPEND _rt64_new_sources "${_rt64_transform_overlay}")
        math(EXPR _rt64_transform_replaced "${_rt64_transform_replaced}+1")
    elseif(_absolute STREQUAL _rt64_cpu_pose_input)
        list(APPEND _rt64_new_sources "${_rt64_cpu_pose_overlay}")
        math(EXPR _rt64_cpu_pose_replaced "${_rt64_cpu_pose_replaced}+1")
    elseif(_absolute STREQUAL _rt64_console_vi_input)
        list(APPEND _rt64_new_sources "${_rt64_console_vi_overlay}")
        math(EXPR _rt64_console_vi_replaced "${_rt64_console_vi_replaced}+1")
    elseif(_absolute STREQUAL _rt64_matrix_trace_input)
        list(APPEND _rt64_new_sources "${_rt64_matrix_trace_overlay}")
        math(EXPR _rt64_matrix_trace_replaced "${_rt64_matrix_trace_replaced}+1")
    elseif(WIN32 AND _absolute STREQUAL _rt64_dynamic_input)
        list(APPEND _rt64_new_sources "${_rt64_dynamic_overlay}")
        math(EXPR _rt64_dynamic_replaced "${_rt64_dynamic_replaced}+1")
    else()
        list(APPEND _rt64_new_sources "${_source}")
    endif()
endforeach()
if(NOT _rt64_present_replaced EQUAL 1)
    message(FATAL_ERROR "Expected one pinned RT64 present source; got ${_rt64_present_replaced}")
endif()
if(NOT _rt64_console_vi_replaced EQUAL 1)
    message(FATAL_ERROR "Expected one pinned RT64 Console VI source; got ${_rt64_console_vi_replaced}")
endif()
if(NOT _rt64_matching_replaced EQUAL 1)
    message(FATAL_ERROR "Expected one pinned RT64 matching source; got ${_rt64_matching_replaced}")
endif()
if(NOT _rt64_projection_replaced EQUAL 1)
    message(FATAL_ERROR "Expected one pinned RT64 projection source; got ${_rt64_projection_replaced}")
endif()
if(NOT _rt64_rotation_replaced EQUAL 1)
    message(FATAL_ERROR "Expected one pinned RT64 math source; got ${_rt64_rotation_replaced}")
endif()
if(NOT _rt64_rigid_body_replaced EQUAL 1)
    message(FATAL_ERROR "Expected one pinned RT64 rigid-body source; got ${_rt64_rigid_body_replaced}")
endif()
if(NOT _rt64_transform_replaced EQUAL 1)
    message(FATAL_ERROR "Expected one pinned RT64 transform source; got ${_rt64_transform_replaced}")
endif()
if(NOT _rt64_cpu_pose_replaced EQUAL 1)
    message(FATAL_ERROR "Expected one pinned RT64 RSP source; got ${_rt64_cpu_pose_replaced}")
endif()
if(NOT _rt64_matrix_trace_replaced EQUAL 1)
    message(FATAL_ERROR "Expected one pinned RT64 workload queue source; got ${_rt64_matrix_trace_replaced}")
endif()
if(WIN32 AND NOT _rt64_dynamic_replaced EQUAL 1)
    message(FATAL_ERROR "Expected one pinned RT64 dynamic loader source; got ${_rt64_dynamic_replaced}")
endif()
set_property(TARGET rt64 PROPERTY SOURCES "${_rt64_new_sources}")
target_sources(rt64 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/pacing_present_probe.cpp")
target_sources(rt64 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/rt64_matrix_trace.cpp")
target_sources(rt64 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/model_interpolation_rt64.cpp")
target_include_directories(rt64 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${TOOIE_RT64_ROOT}/src/common" "${TOOIE_RT64_ROOT}/src/hle" "${TOOIE_RT64_ROOT}/src/render")
# RT64 pins json 3.12 while runtime embeds 3.9. Select one header consistently
# across application/runtime callbacks; mixed private include precedence changes
# nlohmann's ABI namespace and leaves trace/callback symbols unresolvable.
foreach(target TooieFoundation librecomp ultramodern)
    target_include_directories(${target} BEFORE PUBLIC "${TOOIE_RT64_ROOT}/src/contrib")
endforeach()
target_sources(TooieFoundation PRIVATE
    src/native_host_devices.cpp src/artifact_capture.cpp src/graphics_branch_capture.cpp src/tooie_audio_rsp.cpp src/sdl_audio_observation.cpp
    src/frontend_settings.cpp src/frontend_config.cpp src/frontend_app.cpp src/profile_location.cpp src/frontend_cheats.cpp src/frontend_input_preference.cpp
    src/imgui_backend.cpp src/imgui_menu.cpp src/cheat_save_reset.cpp
    generated/audio_mission01/tooie_audio_rsp_generated.cpp)
target_include_directories(TooieFoundation PRIVATE generated/audio_mission01)
if(MSVC AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64|X86)$")
    set_source_files_properties(generated/audio_mission01/tooie_audio_rsp_generated.cpp PROPERTIES COMPILE_OPTIONS "/clang:-msse4.1")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64|i.86)$")
    set_source_files_properties(generated/audio_mission01/tooie_audio_rsp_generated.cpp PROPERTIES COMPILE_OPTIONS "-msse4.1")
endif()
target_compile_definitions(TooieFoundation PUBLIC TOOIE_NATIVE_HOST=1)
target_include_directories(TooieFoundation PRIVATE
    "${TOOIE_RT64_ROOT}/src" "${TOOIE_RT64_ROOT}/src/imgui" "${TOOIE_RT64_ROOT}/src/contrib"
    "${TOOIE_RT64_ROOT}/src/contrib/hlslpp/include" "${TOOIE_RT64_ROOT}/src/contrib/xxHash"
    "${TOOIE_RT64_ROOT}/src/contrib/dxc/inc" "${TOOIE_RT64_ROOT}/src/contrib/plume")

# CPU-only checks of the real pinned renderer paths; no window or profile.
add_executable(graphics_branch_capture_test tests/graphics_branch_capture_test.cpp)
target_link_libraries(graphics_branch_capture_test PRIVATE TooieFoundation)
target_compile_options(graphics_branch_capture_test PRIVATE -UNDEBUG)
target_compile_definitions(graphics_branch_capture_test PRIVATE SDL_MAIN_HANDLED)
target_include_directories(graphics_branch_capture_test PRIVATE
    "${TOOIE_RT64_ROOT}/src" "${TOOIE_RT64_ROOT}/src/contrib"
    "${TOOIE_RT64_ROOT}/src/contrib/hlslpp/include" "${TOOIE_RT64_ROOT}/src/contrib/plume")
add_test(NAME graphics_branch_capture COMMAND graphics_branch_capture_test)
add_executable(rt64_projection_preservation_test tests/rt64_projection_preservation_test.cpp)
target_link_libraries(rt64_projection_preservation_test PRIVATE TooieFoundation)
target_compile_options(rt64_projection_preservation_test PRIVATE -UNDEBUG)
target_include_directories(rt64_projection_preservation_test PRIVATE
    "${TOOIE_RT64_ROOT}/src" "${TOOIE_RT64_ROOT}/src/contrib"
    "${TOOIE_RT64_ROOT}/src/contrib/hlslpp/include" "${TOOIE_RT64_ROOT}/src/contrib/plume")
add_test(NAME rt64_projection_preservation COMMAND rt64_projection_preservation_test)
add_executable(cpu_pose_camera_policy_test tests/cpu_pose_camera_policy_test.cpp)
target_link_libraries(cpu_pose_camera_policy_test PRIVATE TooieFoundation)
target_compile_options(cpu_pose_camera_policy_test PRIVATE -UNDEBUG)
target_include_directories(cpu_pose_camera_policy_test PRIVATE
    "${TOOIE_RT64_ROOT}/src" "${TOOIE_RT64_ROOT}/src/contrib"
    "${TOOIE_RT64_ROOT}/src/contrib/hlslpp/include" "${TOOIE_RT64_ROOT}/src/contrib/plume")
add_test(NAME cpu_pose_camera_policy COMMAND cpu_pose_camera_policy_test)
if(WIN32)
    target_include_directories(graphics_branch_capture_test PRIVATE "${TOOIE_SDL_ROOT}/include")
endif()
if(WIN32)
    if(NOT EXISTS "${TOOIE_SDL_ROOT}/lib/x64/SDL2.dll")
        message(FATAL_ERROR
            "Pinned SDL2 Windows bundle is missing at ${TOOIE_SDL_ROOT}. "
            "Run: python tools/bootstrap_dependencies.py")
    endif()
    target_sources(TooieFoundation PRIVATE src/sdl_import_anchor.cpp)
    file(SHA256 "${TOOIE_SDL_ROOT}/lib/x64/SDL2.dll" _tooie_sdl_build_sha256)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/dependencies.lock.json" _tooie_dependency_lock)
    string(JSON _tooie_sdl_pinned_sha256 GET "${_tooie_dependency_lock}"
        dependencies sdl2_windows files "lib/x64/SDL2.dll")
    set(TOOIE_SDL_OBSERVER_SHA256 "" CACHE STRING "Explicit approved private observer DLL SHA256; empty requires lock pin")
    set(_tooie_sdl_actual_sha256 "${_tooie_sdl_build_sha256}")
    if(NOT TOOIE_SDL_OBSERVER_SHA256 STREQUAL "")
        string(LENGTH "${TOOIE_SDL_OBSERVER_SHA256}" _tooie_sdl_observer_hash_length)
        if(NOT _tooie_sdl_observer_hash_length EQUAL 64 OR NOT TOOIE_SDL_OBSERVER_SHA256 MATCHES "^[0-9a-fA-F]+$")
            message(FATAL_ERROR "TOOIE_SDL_OBSERVER_SHA256 must be exactly 64 hexadecimal characters")
        endif()
        string(TOLOWER "${TOOIE_SDL_OBSERVER_SHA256}" _tooie_sdl_build_sha256)
    endif()
    if(NOT _tooie_sdl_actual_sha256 STREQUAL _tooie_sdl_pinned_sha256 AND
       NOT _tooie_sdl_actual_sha256 STREQUAL _tooie_sdl_build_sha256)
        message(FATAL_ERROR "Configured SDL2.dll differs from lock pin and explicit observer override")
    endif()
    if(TOOIE_SDL_OBSERVER_SHA256 STREQUAL "" AND
       NOT _tooie_sdl_actual_sha256 STREQUAL _tooie_sdl_pinned_sha256)
        message(FATAL_ERROR "Configured SDL2.dll differs from dependencies.lock.json")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${TOOIE_SDL_ROOT}/lib/x64/SDL2.dll" "${CMAKE_CURRENT_SOURCE_DIR}/dependencies.lock.json")
    target_compile_definitions(TooieFoundation PRIVATE TOOIE_SDL_BUILD_SHA256="${_tooie_sdl_build_sha256}")
    target_include_directories(TooieFoundation PRIVATE "${TOOIE_SDL_ROOT}/include")
    target_link_directories(TooieFoundation PUBLIC "${TOOIE_SDL_ROOT}/lib/x64")
    target_link_libraries(TooieFoundation PUBLIC rt64 SDL2 user32 gdi32 ole32 shell32 uuid)
    foreach(dll "${TOOIE_SDL_ROOT}/lib/x64/SDL2.dll" "${TOOIE_DXC_ROOT}/bin/x64/dxcompiler.dll")
        add_custom_command(TARGET TooieRecompiled POST_BUILD COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:TooieRecompiled>")
    endforeach()
else()
    find_package(SDL2 REQUIRED)
    target_link_libraries(TooieFoundation PUBLIC rt64 SDL2::SDL2)
endif()
# Keep the production resource graph to the font the ImGui menu loads and its
# notices. The old RmlUi stylesheet and SVG layout assets are archival inputs.
add_custom_command(TARGET TooieRecompiled POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:TooieRecompiled>/assets"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/assets/InterVariable.ttf" "$<TARGET_FILE_DIR:TooieRecompiled>/assets/InterVariable.ttf"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/third_party/notices/SIL-OFL-1.1.txt" "$<TARGET_FILE_DIR:TooieRecompiled>/assets/SIL-OFL-1.1.txt"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/third_party/notices/FONT_COPYRIGHTS.txt" "$<TARGET_FILE_DIR:TooieRecompiled>/assets/FONT_COPYRIGHTS.txt"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/recompcontrollerdb.txt" "$<TARGET_FILE_DIR:TooieRecompiled>/recompcontrollerdb.txt")
