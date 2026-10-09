cmake_minimum_required(VERSION 3.24)
project(pikmin2_metal LANGUAGES C CXX OBJCXX)
if(IOS)
    enable_language(OBJC) # ios/Pikmin2/main.m, linked by p2_ios_link_check
endif()
option(P2_BUILD_RENDERER "Build the Aurora GX/Metal integration" OFF)

# Untracked per-machine settings, if you keep any — typically P2_DISC_DIR
# pointing at wherever your extracted dump actually lives. Absent in the
# published tree, which defaults to disc/ in the repository root.
include("${CMAKE_CURRENT_SOURCE_DIR}/local.cmake" OPTIONAL)

# Published-tree overrides. All empty here, so this tree behaves as before.
# Name of the iOS Xcode shell project, its source folder, and the xcconfig the
# build generates for it. One variable because those three must agree.
set(P2_IOS_SHELL_NAME "Pikmin2" CACHE STRING "iOS Xcode shell project name")
set(P2_APP_TARGET "" CACHE STRING "Which bring-up target the overrides below apply to")
set(P2_APP_NAME "" CACHE STRING "Override the app bundle and executable name")
set(P2_APP_ID   "" CACHE STRING "Override the app bundle identifier")
# Disc root compiled into the app. This machine's extracted dump by default;
# the published tree uses a relative path, and P2_DISC_DIRECTORY overrides it
# at runtime either way.
set(P2_APP_DISC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/disc"
    CACHE STRING "Disc root compiled into the app")
option(P2_UBSAN "Undefined-behaviour sanitizer on game and host code (report and continue)" OFF)
if(NOT APPLE)
    message(FATAL_ERROR "The initial bootstrap targets macOS and Metal")
endif()
find_package(Python3 3.12 REQUIRED COMPONENTS Interpreter)
# Reference decomp checkout. Probed rather than hardcoded, so the published
# tree (setup.sh puts it in vendor/) and this one both resolve without flags.
set(P2_DECOMP_DIR "" CACHE PATH "Reference Pikmin 2 decomp checkout (empty = autodetect)")
if(NOT P2_DECOMP_DIR)
    foreach(candidate "${CMAKE_CURRENT_SOURCE_DIR}/vendor/pikmin2"
                      "${CMAKE_CURRENT_SOURCE_DIR}/../../pikmin2")
        if(EXISTS "${candidate}/src")
            set(P2_DECOMP_DIR "${candidate}")
            break()
        endif()
    endforeach()
endif()
if(NOT P2_DECOMP_DIR OR NOT EXISTS "${P2_DECOMP_DIR}/src")
    message(FATAL_ERROR
        "Pikmin 2 decompilation not found.\n"
        "Run ./setup.sh to fetch it into vendor/pikmin2, or pass\n"
        "-DP2_DECOMP_DIR=/path/to/pikmin2.")
endif()
set(P2_DISC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/disc" CACHE PATH "Extracted GPVE01 disc")
# Stable path: prepare_source.py rewrites only files whose content changed.
set(P2_PREPARED "${CMAKE_CURRENT_BINARY_DIR}/prepared/current")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/prepare_source.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/scope_switches.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/switch_scope_files.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/jaudio_patches.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/upstream-revision.txt")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/prepare_source.py"
    --source "${P2_DECOMP_DIR}" --output "${P2_PREPARED}"
    COMMAND_ERROR_IS_FATAL ANY)

add_library(p2_host_flags INTERFACE)
target_include_directories(p2_host_flags INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include" "${P2_PREPARED}/include")
target_compile_definitions(p2_host_flags INTERFACE VERSION_GPVE01 TARGET_PC=1)
target_compile_features(p2_host_flags INTERFACE cxx_std_11)
target_compile_options(p2_host_flags INTERFACE
    -fno-exceptions -fno-rtti -fcheck-new -fwrapv -fno-strict-aliasing
    -fvisibility=hidden -fvisibility-inlines-hidden -ffp-contract=off
    -Wno-address-of-temporary -Wno-multichar -Wno-unknown-pragmas -Wno-deprecated-register -Wno-register
    # MWCC-era code falls off the end of non-void functions (~119 sites). Trap
    # instead of returning garbage so an unfixed site fails loudly when reached.
    -fsanitize=return -fsanitize-trap=return
    # Bug classes the port has already cleared to zero; keep them there.
    -Werror=return-type -Werror=uninitialized -Werror=sometimes-uninitialized
    -Werror=conditional-uninitialized -Werror=mismatched-new-delete -Werror=array-bounds
    -Werror=int-to-pointer-cast -Werror=int-to-void-pointer-cast
    -Werror=format -Werror=format-security
    -include "${CMAKE_CURRENT_SOURCE_DIR}/include/p2_host_compat.h")
if(P2_UBSAN)
    # Big-endian disc data is read unaligned on purpose, -fno-rtti rules out vptr
    # and function checks, and -fwrapv already defines signed overflow.
    set(P2_UBSAN_FLAGS -fsanitize=undefined -fno-sanitize=alignment,vptr,function
        -fno-omit-frame-pointer)
    target_compile_options(p2_host_flags INTERFACE ${P2_UBSAN_FLAGS})
    target_link_options(p2_host_flags INTERFACE ${P2_UBSAN_FLAGS})
endif()

# A real native slice of JSystem, linked and checked without any game dump.
add_library(p2_math STATIC
    src/random.cpp
    src/matrix.cpp
    "${P2_PREPARED}/src/JSystem/JMath/random.cpp"
    "${P2_PREPARED}/src/JSystem/JMath/JMATrigonometric.cpp")
target_link_libraries(p2_math PUBLIC p2_host_flags)
add_executable(p2_host_checks tests/host_checks.cpp)
add_library(p2_texture_refs STATIC src/texture_refs.cpp)
target_compile_features(p2_texture_refs PUBLIC cxx_std_11)
target_include_directories(p2_texture_refs PUBLIC include)
target_link_libraries(p2_host_checks PRIVATE p2_math p2_texture_refs)
enable_testing()
add_test(NAME source_switch_compatibility COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/switch_scope_checks.py" "${CMAKE_CXX_COMPILER}" "${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME host_layout_and_math COMMAND p2_host_checks)
add_executable(p2_random_checks tests/random_checks.cpp)
target_link_libraries(p2_random_checks PRIVATE p2_math)
add_test(NAME native_game_random COMMAND p2_random_checks)
add_executable(p2_card_command_checks tests/card_command_checks.cpp)
target_link_libraries(p2_card_command_checks PRIVATE p2_host_flags)
add_test(NAME native_card_commands COMMAND p2_card_command_checks)
add_library(p2_memory STATIC src/memory.cpp src/panic.cpp)
target_link_libraries(p2_memory PRIVATE p2_host_flags)
# Diagnostic logs: the build directory on the Mac; on iOS the working directory,
# which ios_main.cpp sets to the app's Documents folder.
if(IOS)
    set(P2_LOG_DIR "")
else()
    set(P2_LOG_DIR "${CMAKE_CURRENT_BINARY_DIR}/")
endif()
target_compile_definitions(p2_memory PRIVATE P2_PANIC_LOG_PATH="${P2_LOG_DIR}native-panic.log")
add_library(p2_messages STATIC src/messages.cpp)
add_library(p2_threads STATIC src/threads.cpp src/critical.cpp)
target_link_libraries(p2_threads PUBLIC p2_host_flags)
target_link_libraries(p2_memory PRIVATE p2_threads)
target_link_libraries(p2_messages PUBLIC p2_host_flags p2_threads)
add_executable(p2_message_checks tests/message_checks.cpp)
target_link_libraries(p2_message_checks PRIVATE p2_messages)
add_test(NAME native_os_messages COMMAND p2_message_checks)
add_executable(p2_try_lock_checks tests/try_lock_checks.cpp)
target_link_libraries(p2_try_lock_checks PRIVATE p2_memory p2_threads)
add_test(NAME native_try_lock COMMAND p2_try_lock_checks)
set_tests_properties(native_try_lock PROPERTIES TIMEOUT 20)
set_tests_properties(native_os_messages PROPERTIES TIMEOUT 20)
# P2HostScratchScope alone, for host services that must not allocate from game heaps.
add_library(p2_host_scratch STATIC src/host_scratch.cpp)
target_include_directories(p2_host_scratch PUBLIC include)
add_library(p2_heap STATIC src/game_alloc.cpp
    "${P2_PREPARED}/src/JSystem/JKernel/JKRHeap.cpp"
    "${P2_PREPARED}/src/JSystem/JKernel/JKRExpHeap.cpp"
    "${P2_PREPARED}/src/JSystem/JKernel/JKRSolidHeap.cpp"
    "${P2_PREPARED}/src/JSystem/JKernel/JKRDisposer.cpp"
    "${P2_PREPARED}/src/JSystem/JSupport/JSUList.cpp")
target_link_libraries(p2_heap PUBLIC p2_host_flags p2_memory p2_host_scratch)
set_target_properties(p2_heap p2_memory PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED YES)
add_executable(p2_heap_checks tests/heap_checks.cpp
    "${P2_PREPARED}/src/JSystem/JUtility/JUTProcBar.cpp")
target_link_libraries(p2_heap_checks PRIVATE p2_heap)
target_link_options(p2_heap_checks PRIVATE -Wl,-dead_strip)
add_test(NAME native_jkr_heaps COMMAND p2_heap_checks)
add_library(p2_jkr_thread STATIC "${P2_PREPARED}/src/JSystem/JKernel/JKRThread.cpp")
target_link_libraries(p2_jkr_thread PUBLIC p2_heap p2_messages)
add_executable(p2_thread_checks tests/thread_checks.cpp)
target_link_libraries(p2_thread_checks PRIVATE p2_jkr_thread p2_dvd_ripper)
target_link_options(p2_thread_checks PRIVATE -Wl,-dead_strip)
add_test(NAME native_jkr_threads COMMAND p2_thread_checks)
set_tests_properties(native_jkr_threads PROPERTIES TIMEOUT 20)

# Compile-only bring-up target. Not part of the default build until the census
# is clean; no SDK hardware code, original runtime, or DSP/audio backend.
file(GLOB_RECURSE P2_ENGINE_SOURCES CONFIGURE_DEPENDS "${P2_PREPARED}/src/*.cpp")
list(FILTER P2_ENGINE_SOURCES EXCLUDE REGEX "/Dolphin/|/JAudio/|/JStudio_JAudio/")
add_library(p2_engine OBJECT EXCLUDE_FROM_ALL ${P2_ENGINE_SOURCES})
target_link_libraries(p2_engine PRIVATE p2_host_flags)
set_target_properties(p2_engine p2_math PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED YES)
add_custom_target(compile_census
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/compile_census.py"
        --build "${CMAKE_CURRENT_BINARY_DIR}" --source-root "${P2_PREPARED}"
    USES_TERMINAL)

add_executable(pikmin2_bootstrap src/bootstrap.mm)
target_include_directories(pikmin2_bootstrap PRIVATE include)
target_compile_options(pikmin2_bootstrap PRIVATE -fobjc-arc)
target_compile_features(pikmin2_bootstrap PRIVATE cxx_std_17)
target_link_libraries(pikmin2_bootstrap PRIVATE "-framework Cocoa" "-framework Metal" "-framework MetalKit")

add_library(p2_assets STATIC src/assets.cpp)
target_compile_features(p2_assets PUBLIC cxx_std_17)
target_include_directories(p2_assets PUBLIC include)
add_library(p2_stream STATIC
    "${P2_PREPARED}/src/JSystem/JSupport/JSUInputStream.cpp"
    "${P2_PREPARED}/src/JSystem/JSupport/JSUMemoryStream.cpp")
target_link_libraries(p2_stream PUBLIC p2_host_flags)
set_target_properties(p2_stream PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED YES)
add_executable(p2_stream_checks tests/stream_checks.cpp)
add_library(p2_layout_data STATIC src/layout_data.cpp "${P2_PREPARED}/src/JSystem/J2D/J2DManage.cpp")
target_link_libraries(p2_layout_data PUBLIC p2_stream)
target_link_libraries(p2_stream_checks PRIVATE p2_stream p2_assets p2_layout_data)
add_test(NAME native_jssystem_streams COMMAND p2_stream_checks)
add_executable(p2_asset_checks tests/asset_checks.cpp)
target_link_libraries(p2_asset_checks PRIVATE p2_assets)
add_test(NAME asset_reader_bounds COMMAND p2_asset_checks)
add_executable(p2_asset_probe src/asset_probe.cpp)
target_link_libraries(p2_asset_probe PRIVATE p2_assets)
target_compile_definitions(p2_asset_probe PRIVATE P2_ASSET_ROOT="${P2_DISC_DIR}/files")
add_library(p2_dvd STATIC src/dvd.cpp)
target_compile_features(p2_dvd PUBLIC cxx_std_17)
target_include_directories(p2_dvd PUBLIC include "${P2_PREPARED}/include")
target_compile_definitions(p2_dvd PUBLIC VERSION_GPVE01 PRIVATE P2_DISC_ROOT="${P2_DISC_DIR}")
target_link_libraries(p2_dvd PUBLIC p2_assets p2_host_scratch)
add_executable(p2_dvd_checks tests/dvd_checks.cpp)
target_link_libraries(p2_dvd_checks PRIVATE p2_dvd)
add_library(p2_jkr_file STATIC
    "${P2_PREPARED}/src/JSystem/JKernel/JKRDvdFile.cpp"
    "${P2_PREPARED}/src/JSystem/JKernel/JKRFile.cpp"
    "${P2_PREPARED}/src/JSystem/JSupport/JSUFileStream.cpp")
target_link_libraries(p2_jkr_file PUBLIC p2_heap p2_dvd p2_messages p2_stream)
add_executable(p2_file_checks tests/file_checks.cpp)
target_link_libraries(p2_file_checks PRIVATE p2_jkr_file)
target_link_options(p2_file_checks PRIVATE -Wl,-dead_strip)
add_library(p2_dvd_ripper STATIC src/dvd_ripper.cpp
    "${P2_PREPARED}/src/JSystem/JKernel/JKRDvdRipper.cpp")
target_link_libraries(p2_dvd_ripper PUBLIC p2_jkr_file)
set_source_files_properties(src/dvd_ripper.cpp PROPERTIES COMPILE_OPTIONS "-fexceptions")
add_executable(p2_ripper_checks tests/ripper_checks.cpp)
target_link_libraries(p2_ripper_checks PRIVATE p2_dvd_ripper)
target_link_options(p2_ripper_checks PRIVATE -Wl,-dead_strip)
add_library(p2_archive STATIC src/archive.cpp
    "${P2_PREPARED}/src/JSystem/JKernel/JKRArchivePri.cpp"
    "${P2_PREPARED}/src/JSystem/JKernel/JKRArchivePub.cpp"
    "${P2_PREPARED}/src/JSystem/JKernel/JKRMemArchive.cpp"
    "${P2_PREPARED}/src/JSystem/JKernel/JKRFileLoader.cpp"
    "${P2_PREPARED}/src/JSystem/JKernel/JKRFileFinder.cpp")
target_link_libraries(p2_archive PUBLIC p2_heap p2_dvd)
add_library(p2_dvd_worker STATIC src/dvd_worker.cpp
    "${P2_PREPARED}/src/sysGCU/appThread.cpp"
    "${P2_PREPARED}/src/sysGCU/dvdThread.cpp")
target_link_libraries(p2_dvd_worker PUBLIC p2_jkr_thread p2_archive p2_dvd_ripper)
add_executable(p2_dvd_worker_checks tests/dvd_worker_checks.cpp)
target_link_libraries(p2_dvd_worker_checks PRIVATE p2_dvd_worker)
target_link_options(p2_dvd_worker_checks PRIVATE -Wl,-dead_strip)
add_library(p2_aram STATIC src/aram_memory.cpp src/aram.cpp src/aram_ripper.cpp
    "${P2_PREPARED}/src/JSystem/JKernel/JKRDvdAramRipper.cpp"
    "${P2_PREPARED}/src/JSystem/JKernel/JKRAramHeap.cpp"
    "${P2_PREPARED}/src/JSystem/JKernel/JKRAramBlock.cpp"
    "${P2_PREPARED}/src/JSystem/JKernel/JKRAramPiece.cpp")
target_link_libraries(p2_aram PUBLIC p2_jkr_thread p2_assets p2_dvd_ripper)
set_source_files_properties(src/aram.cpp PROPERTIES COMPILE_OPTIONS "-fexceptions")
add_executable(p2_aram_checks tests/aram_checks.cpp)
target_link_libraries(p2_aram_checks PRIVATE p2_aram)
target_link_options(p2_aram_checks PRIVATE -Wl,-dead_strip)
add_library(p2_resource_cache STATIC src/resource_cache.cpp
    "${P2_PREPARED}/src/sysCommonU/node.cpp"
    "${P2_PREPARED}/src/sysGCU/pikmin2AramMgr.cpp")
target_link_libraries(p2_resource_cache PUBLIC p2_aram)
add_executable(p2_resource_cache_checks tests/resource_cache_checks.cpp)
target_link_libraries(p2_resource_cache_checks PRIVATE p2_resource_cache)
target_link_options(p2_resource_cache_checks PRIVATE -Wl,-dead_strip)
set_source_files_properties(src/archive.cpp PROPERTIES COMPILE_OPTIONS "-fexceptions")
target_compile_features(p2_archive PRIVATE cxx_std_17)
add_executable(p2_archive_checks tests/archive_checks.cpp)
target_link_libraries(p2_archive_checks PRIVATE p2_archive)
add_library(p2_font STATIC src/font.cpp)
target_link_libraries(p2_font PUBLIC p2_archive)
add_executable(p2_font_checks tests/font_checks.cpp
    "${P2_PREPARED}/checks/JUTResFontCpu.cpp"
    "${P2_PREPARED}/src/JSystem/JUtility/JUTFontData_Ascfont_fix12.cpp"
    "${P2_PREPARED}/src/JSystem/JUtility/JUTFont.cpp")
target_link_libraries(p2_font_checks PRIVATE p2_font)
target_link_options(p2_font_checks PRIVATE -Wl,-dead_strip)
add_executable(p2_message_resource_checks tests/message_resource_checks.cpp
    "${P2_PREPARED}/src/JSystem/JMessage/processor.cpp"
    "${P2_PREPARED}/src/JSystem/JMessage/control.cpp"
    "${P2_PREPARED}/src/JSystem/JMessage/resource.cpp"
    "${P2_PREPARED}/src/JSystem/JMessage/data.cpp"
    "${P2_PREPARED}/src/JSystem/JGadget/binary.cpp"
    "${P2_PREPARED}/src/JSystem/JGadget/linklist.cpp")
target_link_libraries(p2_message_resource_checks PRIVATE p2_assets p2_host_flags)
target_link_options(p2_message_resource_checks PRIVATE -Wl,-dead_strip)
add_test(NAME native_message_resources COMMAND p2_message_resource_checks "${P2_DISC_DIR}/files")
add_library(p2_particle_data STATIC src/particle_data.cpp)
target_link_libraries(p2_particle_data PUBLIC p2_host_flags)
add_executable(p2_particle_checks tests/particle_checks.cpp
    "${P2_PREPARED}/src/JSystem/JParticle/JPADynamicsBlock.cpp"
    "${P2_PREPARED}/src/JSystem/JParticle/JPAKeyBlock.cpp"
    "${P2_PREPARED}/src/JSystem/JParticle/JPAFieldBlock.cpp"
    "${P2_PREPARED}/src/JSystem/JParticle/JPAMath.cpp"
    "${P2_PREPARED}/src/JSystem/JParticle/JPABaseShape.cpp"
    "${P2_PREPARED}/src/JSystem/JParticle/JPAChildShape.cpp"
    "${P2_PREPARED}/src/JSystem/JParticle/JPAExtraShape.cpp"
    "${P2_PREPARED}/src/JSystem/JParticle/JPAExTexShape.cpp")
target_link_libraries(p2_particle_checks PRIVATE p2_heap p2_math p2_assets p2_particle_data p2_dvd_ripper)
target_link_options(p2_particle_checks PRIVATE -Wl,-dead_strip)
add_executable(p2_j3d_block_checks tests/j3d_block_checks.cpp tests/material_checks.cpp)
add_library(p2_name_table STATIC "${P2_PREPARED}/src/JSystem/JUtility/JUTNameTab.cpp")
target_link_libraries(p2_name_table PUBLIC p2_host_flags)
add_library(p2_material2d STATIC src/material2d.cpp "${P2_PREPARED}/src/JSystem/J2D/J2DMaterialFactory.cpp")
target_link_libraries(p2_material2d PUBLIC p2_heap)
target_compile_options(p2_material2d PRIVATE -ffunction-sections -fdata-sections)
add_executable(p2_material2d_checks tests/material2d_checks.cpp)
target_link_libraries(p2_material2d_checks PRIVATE p2_material2d p2_assets)
target_link_options(p2_material2d_checks PRIVATE -Wl,-dead_strip)
add_test(NAME native_title_2d_materials COMMAND p2_material2d_checks "${P2_DISC_DIR}/files")
add_library(p2_animation STATIC src/animation.cpp "${P2_PREPARED}/src/JSystem/J3D/J3DAnimation.cpp"
    "${P2_PREPARED}/src/JSystem/J2D/J2DAnimation.cpp")
target_link_libraries(p2_animation PUBLIC p2_heap p2_math p2_name_table)
target_compile_options(p2_animation PRIVATE -ffunction-sections -fdata-sections)
add_executable(p2_animation_checks tests/animation_checks.cpp)
target_link_libraries(p2_animation_checks PRIVATE p2_animation p2_archive)
target_link_options(p2_animation_checks PRIVATE -Wl,-dead_strip)
target_link_libraries(p2_j3d_block_checks PRIVATE p2_archive p2_name_table p2_texture_refs)
add_library(p2_model_data STATIC src/model_data.cpp
    src/vertex.cpp
    src/skin.cpp
    src/shape.cpp
    src/material.cpp
    "${P2_PREPARED}/src/JSystem/J3D/J3DMaterialFactory.cpp"
    "${P2_PREPARED}/src/JSystem/J3D/J3DTevs.cpp"
    "${P2_PREPARED}/src/JSystem/J3D/J3DShapeFactory.cpp"
    "${P2_PREPARED}/src/JSystem/J3D/J3DVertex.cpp"
    "${P2_PREPARED}/src/JSystem/J3D/J3DJointFactory.cpp"
    "${P2_PREPARED}/src/JSystem/J3D/J3DJoint.cpp"
    "${P2_PREPARED}/src/JSystem/J3D/J3DTransform.cpp")
target_link_libraries(p2_model_data PUBLIC p2_heap p2_math)
target_compile_options(p2_model_data PRIVATE -ffunction-sections -fdata-sections)
target_link_libraries(p2_j3d_block_checks PRIVATE p2_model_data p2_stream)
# Use the original SDK command-buffer writer for CPU checks. The renderer build
# provides GD through Aurora instead; this test library is never linked there.
add_library(p2_gd_buffer STATIC "${P2_PREPARED}/src/Dolphin/gd/GDBase.c")
set_source_files_properties("${P2_PREPARED}/src/Dolphin/gd/GDBase.c" PROPERTIES LANGUAGE CXX)
target_link_libraries(p2_gd_buffer PRIVATE p2_host_flags)
target_link_libraries(p2_j3d_block_checks PRIVATE p2_gd_buffer)
target_link_options(p2_j3d_block_checks PRIVATE -Wl,-dead_strip)
if(EXISTS "${P2_DISC_DIR}/sys/main.dol")
    add_test(NAME native_particle_resources COMMAND p2_particle_checks "${P2_DISC_DIR}")
    add_test(NAME native_font_lookup COMMAND p2_font_checks "${P2_DISC_DIR}")
    add_test(NAME native_resource_cache COMMAND p2_resource_cache_checks "${P2_DISC_DIR}")
    set_tests_properties(native_resource_cache PROPERTIES TIMEOUT 30)
    add_test(NAME native_aram_resources COMMAND p2_aram_checks "${P2_DISC_DIR}")
    set_tests_properties(native_aram_resources PROPERTIES TIMEOUT 30)
    add_test(NAME native_game_dvd_worker COMMAND p2_dvd_worker_checks "${P2_DISC_DIR}")
    set_tests_properties(native_game_dvd_worker PROPERTIES TIMEOUT 30)
    add_test(NAME native_jkr_title_loading_task COMMAND p2_thread_checks "${P2_DISC_DIR}")
    set_tests_properties(native_jkr_title_loading_task PROPERTIES TIMEOUT 20)
    add_test(NAME native_jkr_file_stream COMMAND p2_file_checks "${P2_DISC_DIR}")
    set_tests_properties(native_jkr_file_stream PROPERTIES TIMEOUT 20)
    add_test(NAME native_jkr_dvd_ripper COMMAND p2_ripper_checks "${P2_DISC_DIR}")
    set_tests_properties(native_jkr_dvd_ripper PROPERTIES TIMEOUT 30)
    add_test(NAME native_jkr_title_archives COMMAND p2_archive_checks "${P2_DISC_DIR}")
    add_test(NAME native_title_model_blocks COMMAND p2_j3d_block_checks "${P2_DISC_DIR}")
    add_test(NAME native_title_animation COMMAND p2_animation_checks "${P2_DISC_DIR}")
    add_test(NAME native_title_layout_streams COMMAND p2_stream_checks "${P2_DISC_DIR}/files")
    add_test(NAME verify_us_dump COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_dump.py" "${P2_DISC_DIR}")
    add_test(NAME native_pikmin_and_title_assets COMMAND p2_asset_probe)
    add_test(NAME native_dvd_file_access COMMAND p2_dvd_checks)
endif()

if(P2_BUILD_RENDERER)
    include(cmake/renderer.cmake)
    include(cmake/game_link_audit.cmake)
endif()

add_executable(p2_condition_checks tests/condition_checks.cpp)
target_link_libraries(p2_condition_checks PRIVATE p2_memory p2_threads p2_host_flags)
add_test(NAME native_os_conditions COMMAND p2_condition_checks)
set_tests_properties(native_os_conditions PROPERTIES TIMEOUT 20)

add_library(p2_gx_math STATIC src/gx_math.cpp)
target_link_libraries(p2_gx_math PUBLIC p2_host_flags)
add_executable(p2_gx_math_checks tests/gx_math_checks.cpp)
target_link_libraries(p2_gx_math_checks PRIVATE p2_gx_math)
add_test(NAME native_display_calculations COMMAND p2_gx_math_checks)

add_library(p2_video STATIC src/video.cpp)
target_link_libraries(p2_video PUBLIC p2_host_flags PRIVATE p2_threads)
add_executable(p2_video_checks tests/video_checks.cpp)
target_link_libraries(p2_video_checks PRIVATE p2_video p2_messages)
add_test(NAME native_video_retrace COMMAND p2_video_checks)
set_tests_properties(native_video_retrace PROPERTIES TIMEOUT 10)

add_library(p2_platform STATIC src/platform.cpp)
target_link_libraries(p2_platform PUBLIC p2_host_flags)

add_executable(p2_sleep_checks tests/sleep_checks.cpp)
target_link_libraries(p2_sleep_checks PRIVATE p2_threads)
add_test(NAME native_thread_timed_wait COMMAND p2_sleep_checks)
set_tests_properties(native_thread_timed_wait PROPERTIES TIMEOUT 10)

# Exercise the original upload routine against an absent native GBA transport.
add_executable(p2_gba_checks tests/gba_checks.cpp
    "${P2_PREPARED}/src/plugProjectEbisawaU/ebiCardEReader.cpp")
target_link_libraries(p2_gba_checks PRIVATE p2_platform)
target_compile_options(p2_gba_checks PRIVATE -ffunction-sections -fdata-sections)
target_link_options(p2_gba_checks PRIVATE -Wl,-dead_strip)
add_test(NAME native_ereader_disconnected COMMAND p2_gba_checks)
set_tests_properties(native_ereader_disconnected PROPERTIES TIMEOUT 5)

add_executable(p2_panic_checks tests/panic_checks.cpp
    "${P2_PREPARED}/src/JSystem/JUtility/JUTException.cpp")
target_link_libraries(p2_panic_checks PRIVATE p2_heap p2_jkr_thread)
target_compile_options(p2_panic_checks PRIVATE -ffunction-sections -fdata-sections)
target_link_options(p2_panic_checks PRIVATE -Wl,-dead_strip)
add_test(NAME native_panic_diagnostics COMMAND p2_panic_checks)
set_tests_properties(native_panic_diagnostics PROPERTIES TIMEOUT 10)

add_executable(p2_critical_checks tests/critical_checks.cpp
    "${P2_PREPARED}/src/JSystem/J3D/J3DPacket.cpp")
target_link_libraries(p2_critical_checks PRIVATE p2_threads p2_gd_buffer p2_platform p2_heap)
target_compile_options(p2_critical_checks PRIVATE -ffunction-sections -fdata-sections)
target_link_options(p2_critical_checks PRIVATE -Wl,-dead_strip)
add_test(NAME native_graphics_critical_sections COMMAND p2_critical_checks)
set_tests_properties(native_graphics_critical_sections PROPERTIES TIMEOUT 20)

add_library(p2_reset STATIC src/reset.cpp src/crash.cpp)
target_link_libraries(p2_reset PUBLIC p2_host_flags)
add_executable(p2_reset_checks tests/reset_checks.cpp)
target_link_libraries(p2_reset_checks PRIVATE p2_reset)
add_test(NAME native_process_reset COMMAND p2_reset_checks)
set_tests_properties(native_process_reset PROPERTIES TIMEOUT 15)
