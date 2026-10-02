# BlueWake for Linux (x86-64): the unchanged host, GXRuntime/Aurora (Dawn on
# Vulkan, SDL3) and the donor DSP, as windows/CMakeLists.txt builds them for
# Windows. The Linux-specific sources are the entry shim and the disc setup in
# src/; the settings menu is windows/src/win_settings.cpp, which builds on
# both.
#
# Built by scripts/linux/build.py with clang; see docs/LINUX.md.
cmake_minimum_required(VERSION 3.25)
project(bluewake_linux LANGUAGES C CXX)

if(NOT UNIX OR APPLE)
    message(FATAL_ERROR "linux/ builds BlueWake for Linux only")
endif()

get_filename_component(BLUEWAKE_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(GXRUNTIME_DIR ${BLUEWAKE_REPO_ROOT}/ref/recompcore/GXRuntime)
set(BLUEWAKE_HOST_SRC ${BLUEWAKE_REPO_ROOT}/runtime/host/src)
set(BLUEWAKE_WINDOWS_SRC ${BLUEWAKE_REPO_ROOT}/windows/src)

option(BLUEWAKE_LINUX_DEVELOPER_TRACING "Compile BLUEWAKE_TRACE_* probes into the host" OFF)

add_compile_options("-ffile-prefix-map=${BLUEWAKE_REPO_ROOT}/=")
add_compile_definitions(_GNU_SOURCE)

# The app finds its runtime libraries (Dawn, SDL3) beside itself.
set(CMAKE_BUILD_RPATH_USE_ORIGIN ON)
set(CMAKE_BUILD_RPATH "$ORIGIN")
set(CMAKE_INSTALL_RPATH "$ORIGIN")
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_subdirectory(${GXRUNTIME_DIR} gxruntime_build EXCLUDE_FROM_ALL)

set(BLUEWAKE_HOST_SOURCES
    ${BLUEWAKE_HOST_SRC}/main.c
    ${BLUEWAKE_HOST_SRC}/aram_dma.c
    ${BLUEWAKE_HOST_SRC}/audio_capture.c
    ${BLUEWAKE_HOST_SRC}/ipl_sram.c
    ${BLUEWAKE_HOST_SRC}/card_runtime.c
    ${BLUEWAKE_HOST_SRC}/callback_delivery.c
    ${BLUEWAKE_HOST_SRC}/cycle_domain.c
    ${BLUEWAKE_HOST_SRC}/delivery_digest.c
    ${BLUEWAKE_HOST_SRC}/edge_intercepts.c
    ${BLUEWAKE_HOST_SRC}/return_census.c
    ${BLUEWAKE_HOST_SRC}/interrupt_sources.c
    ${BLUEWAKE_HOST_SRC}/cold_fallback.c
    ${BLUEWAKE_HOST_SRC}/external_memory.c
    ${BLUEWAKE_HOST_SRC}/fpu_context.c
    ${BLUEWAKE_HOST_SRC}/pad_event_schedule.c
    ${BLUEWAKE_HOST_SRC}/pad_waypoint.c
    ${BLUEWAKE_HOST_SRC}/pad_wire.c
    ${BLUEWAKE_HOST_SRC}/rel_scratch_allocator.c
    ${BLUEWAKE_HOST_SRC}/scheduler_contract.c
    ${BLUEWAKE_HOST_SRC}/game_options.c
    ${BLUEWAKE_HOST_SRC}/fast_load.c
    ${BLUEWAKE_HOST_SRC}/jump_button.c
    ${BLUEWAKE_HOST_SRC}/climb.c
    ${BLUEWAKE_HOST_SRC}/simulation_mode.c
    ${BLUEWAKE_HOST_SRC}/sprint.c
    ${BLUEWAKE_HOST_SRC}/quick_doors.c
    ${BLUEWAKE_HOST_SRC}/haptics.c
    ${BLUEWAKE_HOST_SRC}/fps_watch.c
    ${BLUEWAKE_HOST_SRC}/draw_tags.c
    ${BLUEWAKE_HOST_SRC}/mouse_camera.c
    ${BLUEWAKE_HOST_SRC}/save_state.c)

add_executable(BlueWake
    src/linux_entry.c
    src/linux_disc.c
    ${BLUEWAKE_WINDOWS_SRC}/win_settings.cpp
    ${BLUEWAKE_REPO_ROOT}/apple/ios/src/disc_import.c
    ${BLUEWAKE_HOST_SOURCES})
set_source_files_properties(${BLUEWAKE_HOST_SRC}/main.c PROPERTIES
    COMPILE_DEFINITIONS "main=bluewake_host_main")
target_link_libraries(BlueWake PRIVATE gxruntime GXRuntime::aurora ZLIB::ZLIB
    ${CMAKE_DL_LIBS} pthread m)
set_target_properties(BlueWake PROPERTIES LINKER_LANGUAGE CXX OUTPUT_NAME BlueWake
    ENABLE_EXPORTS ON)
target_include_directories(BlueWake PRIVATE
    ${BLUEWAKE_WINDOWS_SRC}
    ${GXRUNTIME_DIR}/include
    ${BLUEWAKE_REPO_ROOT}/apple/ios/src
    ${BLUEWAKE_REPO_ROOT}/ref/recompcore/Source/Core/Core/PowerPC/StaticRecomp)
target_compile_definitions(BlueWake PRIVATE
    DOLRECOMP_CPU_HEADER="core/cpu.h"
    BLUEWAKE_ENABLE_DEVELOPER_TRACING=$<BOOL:${BLUEWAKE_LINUX_DEVELOPER_TRACING}>
    BLUEWAKE_EDGE_CENSUS=0
    BLUEWAKE_LINUX=1)

set(_donor ${BLUEWAKE_REPO_ROOT}/ref/recompcore)
set(_dsp ${_donor}/Source/Core/Core/DSP)
add_library(bluewake_dsp_donor STATIC
    ${_dsp}/DSPAccelerator.cpp ${_dsp}/DSPAnalyzer.cpp ${_dsp}/DSPAssembler.cpp
    ${_dsp}/DSPCaptureLogger.cpp ${_dsp}/DSPCodeUtil.cpp ${_dsp}/DSPCore.cpp
    ${_dsp}/DSPDisassembler.cpp ${_dsp}/DSPHWInterface.cpp ${_dsp}/DSPMemoryMap.cpp
    ${_dsp}/DSPStacks.cpp ${_dsp}/DSPTables.cpp ${_dsp}/LabelMap.cpp
    ${_dsp}/Interpreter/DSPIntArithmetic.cpp ${_dsp}/Interpreter/DSPIntBranch.cpp
    ${_dsp}/Interpreter/DSPIntExtOps.cpp ${_dsp}/Interpreter/DSPIntLoadStore.cpp
    ${_dsp}/Interpreter/DSPIntMisc.cpp ${_dsp}/Interpreter/DSPIntMultiplier.cpp
    ${_dsp}/Interpreter/DSPIntTables.cpp ${_dsp}/Interpreter/DSPInterpreter.cpp
    ${_dsp}/Jit/DSPEmitterBase.cpp
    ${_donor}/Source/Core/Core/HW/DSPHLE/DSPHLE.cpp
    ${_donor}/Source/Core/Core/HW/DSPHLE/MailHandler.cpp
    ${_donor}/Source/Core/Core/HW/DSPHLE/UCodes/AESnd.cpp
    ${_donor}/Source/Core/Core/HW/DSPHLE/UCodes/ASnd.cpp
    ${_donor}/Source/Core/Core/HW/DSPHLE/UCodes/AX.cpp
    ${_donor}/Source/Core/Core/HW/DSPHLE/UCodes/AXWii.cpp
    ${_donor}/Source/Core/Core/HW/DSPHLE/UCodes/CARD.cpp
    ${_donor}/Source/Core/Core/HW/DSPHLE/UCodes/GBA.cpp
    ${_donor}/Source/Core/Core/HW/DSPHLE/UCodes/INIT.cpp
    ${_donor}/Source/Core/Core/HW/DSPHLE/UCodes/ROM.cpp
    ${_donor}/Source/Core/Core/HW/DSPHLE/UCodes/UCodes.cpp
    ${_donor}/Source/Core/Core/HW/DSPHLE/UCodes/Zelda.cpp
    ${_donor}/Source/Core/Core/HW/DSPHLE/UCodes/ZeldaUCodesTable.cpp
    ${BLUEWAKE_REPO_ROOT}/apple/ios/src/dsp_common_shim.cpp)
target_include_directories(bluewake_dsp_donor PUBLIC
    ${_donor}/Source/Core ${_donor}/Source ${_donor}/Externals
    ${_donor}/Externals/xxhash/xxHash
    ${BLUEWAKE_REPO_ROOT}/apple/ios/dsp_generated)
# No _M_X86_64: BlueWake runs the DSP interpreter everywhere (DSPEmitterNull).
target_compile_definitions(bluewake_dsp_donor PUBLIC
    _ARCH_64=1 FMT_EXCEPTIONS=0 __STDC_CONSTANT_MACROS __STDC_LIMIT_MACROS)
target_compile_features(bluewake_dsp_donor PUBLIC cxx_std_23)
target_compile_options(bluewake_dsp_donor PRIVATE -fno-strict-aliasing -w)
target_link_libraries(bluewake_dsp_donor PUBLIC fmt::fmt xxhash ZLIB::ZLIB)

add_library(bluewake_dsp_adapter STATIC
    ${BLUEWAKE_HOST_SRC}/dsp_adapter.cpp
    ${BLUEWAKE_HOST_SRC}/dsp_adapter_c.cpp
    ${BLUEWAKE_HOST_SRC}/dsp_adapter_donor_stubs.cpp
    ${BLUEWAKE_HOST_SRC}/dsp_hle_backend.cpp)
target_link_libraries(bluewake_dsp_adapter PUBLIC bluewake_dsp_donor)
target_compile_definitions(BlueWake PRIVATE BLUEWAKE_HAS_DSP_ADAPTER=1)
target_link_libraries(BlueWake PRIVATE bluewake_dsp_adapter)

add_executable(bluewake_disc_extract
    ${BLUEWAKE_REPO_ROOT}/scripts/ios/disc_extract.c
    ${BLUEWAKE_REPO_ROOT}/apple/ios/src/disc_import.c)
target_include_directories(bluewake_disc_extract PRIVATE ${BLUEWAKE_REPO_ROOT}/apple/ios/src)

# The app folder: the executable, the shared libraries it loads (when Dawn or
# SDL3 are shared) and Aurora's bundled pipeline cache. The builder adds the
# game module and the prepared game files.
set(BLUEWAKE_PIPELINE_SEED "${BLUEWAKE_REPO_ROOT}/apple/ios/resources/initial_pipeline_cache.db")
add_custom_command(TARGET BlueWake POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${BLUEWAKE_PIPELINE_SEED}" $<TARGET_FILE_DIR:BlueWake>
    VERBATIM)
foreach(_lib dawn::webgpu_dawn SDL3::SDL3-shared)
    if(TARGET ${_lib})
        get_target_property(_type ${_lib} TYPE)
        if(_type STREQUAL "SHARED_LIBRARY")
            add_custom_command(TARGET BlueWake POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:${_lib}> $<TARGET_SONAME_FILE:${_lib}> $<TARGET_FILE_DIR:BlueWake>
                VERBATIM)
        endif()
    endif()
endforeach()
