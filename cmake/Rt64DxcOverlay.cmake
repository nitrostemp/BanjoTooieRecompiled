# Materialize the pinned RT64 top-level CMake file into the build tree so its
# Windows shader compiler and import library can use the separately pinned
# official DXC release. RT64's source checkout remains immutable.
set(TOOIE_DXC_ROOT "${TOOIE_DEPS_ROOT}/DXC-v1.8.2502-Windows" CACHE PATH
    "Official pinned Windows DirectX Shader Compiler release")
set(_dxc_files "bin/x64/dxc.exe;bin/x64/dxcompiler.dll;lib/x64/dxcompiler.lib")
set(_dxc_expected_hashes "ae4bf100b8c64ab03cdc0ee5996d5f212e96c59d6d37340359342d714ac6c004;f79943b02f73b621421b58a569fd6ee120d0023fb6bfa23844ecad6b00d6a295;c3ce5cab858f13dff3e1440471c48b29829ce0f43d0fe27525ef599a21562131")
foreach(_dxc_file _dxc_expected IN ZIP_LISTS _dxc_files _dxc_expected_hashes)
    if(NOT EXISTS "${TOOIE_DXC_ROOT}/${_dxc_file}")
        message(FATAL_ERROR "Pinned DXC is missing: ${_dxc_file}; run python tools/bootstrap_dependencies.py --only dxc_windows")
    endif()
    file(SHA256 "${TOOIE_DXC_ROOT}/${_dxc_file}" _dxc_actual)
    if(NOT _dxc_actual STREQUAL _dxc_expected)
        message(FATAL_ERROR "Pinned DXC hash mismatch: ${_dxc_file}")
    endif()
endforeach()
set(TOOIE_DXC_TOOL_ROOT "${CMAKE_BINARY_DIR}/tooie_dxc_tools")
file(MAKE_DIRECTORY "${TOOIE_DXC_TOOL_ROOT}")
configure_file("${TOOIE_DXC_ROOT}/bin/x64/dxc.exe"
    "${TOOIE_DXC_TOOL_ROOT}/dxc.exe" COPYONLY)
configure_file("${TOOIE_DXC_ROOT}/bin/x64/dxcompiler.dll"
    "${TOOIE_DXC_TOOL_ROOT}/dxcompiler.dll" COPYONLY)
if(EXISTS "${TOOIE_DXC_TOOL_ROOT}/dxil.dll")
    message(FATAL_ERROR "DXC tool directory must not contain dxil.dll; internal validator test is invalid")
endif()

set(_rt64_cmake_input "${TOOIE_RT64_ROOT}/CMakeLists.txt")
file(SHA256 "${_rt64_cmake_input}" _rt64_cmake_hash)
if(NOT _rt64_cmake_hash STREQUAL "4d6cb49be103302c019405794995aac6b526b759f257f4786a7a2a00d4de3fa2")
    message(FATAL_ERROR "Pinned RT64 CMakeLists.txt changed; review DXC overlay")
endif()
file(READ "${_rt64_cmake_input}" _rt64_cmake)
string(REPLACE "project(rt64 LANGUAGES C CXX)"
    "project(rt64 LANGUAGES C CXX)\nset(PROJECT_SOURCE_DIR \"${TOOIE_RT64_ROOT}\")"
    _rt64_cmake "${_rt64_cmake}")
string(REPLACE "set (DXC \"\${PROJECT_SOURCE_DIR}/src/contrib/dxc/bin/x64/dxc.exe\")"
    "set (DXC \"${TOOIE_DXC_TOOL_ROOT}/dxc.exe\")" _rt64_cmake "${_rt64_cmake}")
string(REPLACE "configure_file(\"\${PROJECT_SOURCE_DIR}/src/contrib/dxc/bin/x64/dxcompiler.dll\" \"dxcompiler.dll\" COPYONLY)"
    "configure_file(\"${TOOIE_DXC_ROOT}/bin/x64/dxcompiler.dll\" \"dxcompiler.dll\" COPYONLY)"
    _rt64_cmake "${_rt64_cmake}")
string(REPLACE "    configure_file(\"\${PROJECT_SOURCE_DIR}/src/contrib/dxc/bin/x64/dxil.dll\" \"dxil.dll\" COPYONLY)\n"
    "" _rt64_cmake "${_rt64_cmake}")
string(REPLACE "\${PROJECT_SOURCE_DIR}/src/contrib/dxc/lib/x64/dxcompiler.lib"
    "${TOOIE_DXC_ROOT}/lib/x64/dxcompiler.lib" _rt64_cmake "${_rt64_cmake}")
foreach(_relative IN ITEMS
        src/tools/file_to_c src/contrib/re-spirv
        src/contrib/nativefiledialog-extended src/contrib/zstd/build/cmake
        src/contrib/plume src/tools/texture_hasher src/tools/texture_packer
        src/tools/spirv_cross_msl)
    string(REPLACE "add_subdirectory(${_relative})"
        "add_subdirectory(\"${TOOIE_RT64_ROOT}/${_relative}\" \"${CMAKE_BINARY_DIR}/rt64/${_relative}\")"
        _rt64_cmake "${_rt64_cmake}")
endforeach()
set(_tooie_rt64_cmake_overlay "${CMAKE_BINARY_DIR}/tooie_rt64_cmake_overlay")
file(MAKE_DIRECTORY "${_tooie_rt64_cmake_overlay}")
set(_rt64_cmake_output "${_tooie_rt64_cmake_overlay}/CMakeLists.txt")
if(EXISTS "${_rt64_cmake_output}")
    file(READ "${_rt64_cmake_output}" _rt64_cmake_old)
endif()
if(NOT "${_rt64_cmake_old}" STREQUAL "${_rt64_cmake}")
    file(WRITE "${_rt64_cmake_output}" "${_rt64_cmake}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_rt64_cmake_input}" "${CMAKE_CURRENT_SOURCE_DIR}/dependencies.lock.json"
    "${TOOIE_DXC_ROOT}/bin/x64/dxc.exe" "${TOOIE_DXC_ROOT}/bin/x64/dxcompiler.dll"
    "${TOOIE_DXC_ROOT}/lib/x64/dxcompiler.lib")
