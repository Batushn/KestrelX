cmake_minimum_required(VERSION 3.24)
project(KestrelX VERSION 0.5.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR "KestrelX targets Windows x64 (native x86-64 execution + D3D12 Ultimate).")
endif()

add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    _CRT_SECURE_NO_WARNINGS
)

if(MSVC)
    add_compile_options(/W4 /permissive- /Zc:__cplusplus /EHsc /utf-8)
endif()

add_library(kestrel_core STATIC
    src/common/types.cpp
    src/loader/game_config.cpp
    src/loader/pe_image.cpp
    src/loader/pe_loader.cpp
    src/cpu/veh_shim.cpp
    src/vfs/overlay_vfs.cpp
    src/hle/registry.cpp
    src/hle/kernelx.cpp
    src/hle/xgameruntime.cpp
    src/hle/dstorage.cpp
    src/hle/d3d12x.cpp
    src/hle/window_host.cpp
    src/hle/gameinput.cpp
    src/hle/xaudio2.cpp
    src/gpu/rdna2_isa.cpp
    src/gpu/shader_container.cpp
    src/gpu/shader_cache.cpp
    src/gpu/ir.cpp
    src/gpu/rdna2_recomp.cpp
    src/gpu/dxc_compiler.cpp
    src/gpu/gpu_runner.cpp
    src/gpu/gfx10_tiling.cpp
)
target_include_directories(kestrel_core PUBLIC include)
target_link_libraries(kestrel_core PUBLIC d3d12 dxgi dxguid ole32 user32 gdi32)

add_executable(kestrel src/cli/main.cpp)
target_link_libraries(kestrel PRIVATE kestrel_core)

# Synthetic console import libraries so our clean-room test title links against
# console DLL names (kernelx.dll, xgameruntime.dll, d3d12_xs.dll, gameinput.dll,
# xaudio2_9.dll) without needing any proprietary SDK files.
set(STUB_LIB_DIR "${CMAKE_BINARY_DIR}/stub_libs")
file(MAKE_DIRECTORY "${STUB_LIB_DIR}")

function(add_stub_import_lib name def_file)
    set(out_lib "${STUB_LIB_DIR}/${name}.lib")
    add_custom_command(
        OUTPUT "${out_lib}"
        COMMAND lib.exe /NOLOGO /MACHINE:X64 /DEF:"${CMAKE_SOURCE_DIR}/${def_file}" /OUT:"${out_lib}"
        DEPENDS "${CMAKE_SOURCE_DIR}/${def_file}"
        COMMENT "Generating import library ${name}.lib from ${def_file}"
    )
    add_custom_target(stub_${name} DEPENDS "${out_lib}")
endfunction()

add_stub_import_lib(kernelx tests/test_title/defs/kernelx.def)
add_stub_import_lib(xgameruntime tests/test_title/defs/xgameruntime.def)
add_stub_import_lib(d3d12_xs tests/test_title/defs/d3d12_xs.def)
add_stub_import_lib(dstorage_xs tests/test_title/defs/dstorage_xs.def)
add_stub_import_lib(gameinput tests/test_title/defs/gameinput.def)
add_stub_import_lib(xaudio2_9 tests/test_title/defs/xaudio2_9.def)

# Clean-room Xbox-ABI guest DLL and executable for end-to-end loader/HLE/D3D12.X verification
add_library(kestrel_test_dll SHARED tests/test_title/guest_dll.cpp)
target_include_directories(kestrel_test_dll PRIVATE tests/test_title)
target_compile_options(kestrel_test_dll PRIVATE /GS-)
target_link_options(kestrel_test_dll PRIVATE /ENTRY:GuestDllMain /NODEFAULTLIB /DYNAMICBASE)
set_target_properties(kestrel_test_dll PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test_title"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test_title"
)

add_executable(kestrel_test_title tests/test_title/main.cpp)
target_include_directories(kestrel_test_title PRIVATE tests/test_title)
target_compile_options(kestrel_test_title PRIVATE /GS-)
target_link_options(kestrel_test_title PRIVATE /ENTRY:KestrelGuestEntry /DYNAMICBASE)
add_dependencies(kestrel_test_title stub_kernelx stub_xgameruntime stub_d3d12_xs stub_dstorage_xs stub_gameinput stub_xaudio2_9 kestrel_test_dll)
target_link_libraries(kestrel_test_title PRIVATE
    kestrel_test_dll
    "${STUB_LIB_DIR}/kernelx.lib"
    "${STUB_LIB_DIR}/xgameruntime.lib"
    "${STUB_LIB_DIR}/d3d12_xs.lib"
    "${STUB_LIB_DIR}/dstorage_xs.lib"
    "${STUB_LIB_DIR}/gameinput.lib"
    "${STUB_LIB_DIR}/xaudio2_9.lib"
    vcruntime
)
set_target_properties(kestrel_test_title PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test_title"
)

add_custom_command(TARGET kestrel_test_title POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/tests/fixtures/MicrosoftGame.config"
        "${CMAKE_BINARY_DIR}/test_title/MicrosoftGame.config"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/test_title/data"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/tests/fixtures/readonly.txt"
        "${CMAKE_BINARY_DIR}/test_title/data/readonly.txt"
)

enable_testing()
add_executable(kestrel_tests tests/test_runner.cpp)
target_link_libraries(kestrel_tests PRIVATE kestrel_core)
add_dependencies(kestrel_tests kestrel_test_title)
target_compile_definitions(kestrel_tests PRIVATE
    KESTREL_TEST_TITLE_DIR="${CMAKE_BINARY_DIR}/test_title"
)
add_test(NAME kestrel_unit_and_gpu_tests COMMAND kestrel_tests)
