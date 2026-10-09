# Aurora (encounter/aurora, MIT) supplies the GX/Metal layer. It is not
# vendored. Two trees satisfy it differently, so probe rather than hardcode:
#   published tree     setup.sh clones it to vendor/aurora at the pinned commit
#   development tree   an existing local checkout, e.g. dusklight's submodule
# Pass -DP2_AURORA_SOURCE=/path to override either.
set(P2_AURORA_SOURCE "" CACHE PATH "Aurora repository (empty = autodetect)")
if(NOT P2_AURORA_SOURCE)
    foreach(candidate
            "${CMAKE_CURRENT_SOURCE_DIR}/vendor/aurora"
            "${CMAKE_CURRENT_SOURCE_DIR}/../../dusklight/extern/aurora")
        if(EXISTS "${candidate}/CMakeLists.txt")
            set(P2_AURORA_SOURCE "${candidate}")
            break()
        endif()
    endforeach()
endif()
if(NOT P2_AURORA_SOURCE OR NOT EXISTS "${P2_AURORA_SOURCE}/CMakeLists.txt")
    message(FATAL_ERROR
        "Aurora not found.\n"
        "Run ./setup.sh to fetch it into vendor/aurora, or pass\n"
        "-DP2_AURORA_SOURCE=/path/to/aurora.")
endif()
message(STATUS "Pikmin 2: Aurora from ${P2_AURORA_SOURCE}")
# Stable path: prepare_renderer.py rewrites only files whose content changed.
set(P2_RENDERER "${CMAKE_CURRENT_BINARY_DIR}/renderer-source/current")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/prepare_renderer.py")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/prepare_renderer.py"
    --source "${P2_AURORA_SOURCE}" --output "${P2_RENDERER}" COMMAND_ERROR_IS_FATAL ANY)
set(AURORA_ENABLE_DVD OFF CACHE BOOL "Use the port's extracted-disc service" FORCE)
set(AURORA_ENABLE_CARD ON CACHE BOOL "Use Aurora local memory-card storage" FORCE)
set(AURORA_ENABLE_THP ON CACHE BOOL "Build the bounded native THP video decoder" FORCE)
set(AURORA_BUILD_TESTS OFF CACHE BOOL "Build upstream Aurora tests" FORCE)
set(AURORA_SDL3_PROVIDER system CACHE STRING "Use installed SDL3")
# Extract directly into a content-addressed directory. FetchContent's temporary
# directory rename is denied by some host filesystem configurations.
function(p2_local_renderer_dependency name archive expected_hash)
    if(NOT EXISTS "${archive}")
        return()
    endif()
    file(SHA256 "${archive}" archive_hash)
    if(expected_hash AND NOT archive_hash STREQUAL expected_hash)
        message(FATAL_ERROR "Unexpected checksum for ${archive}")
    endif()
    set(unpacked "${CMAKE_CURRENT_BINARY_DIR}/local-renderer-deps/${name}-${archive_hash}")
    if(NOT EXISTS "${unpacked}/.extracted")
        file(MAKE_DIRECTORY "${unpacked}")
        file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${unpacked}")
        file(WRITE "${unpacked}/.extracted" "${archive_hash}")
    endif()
    file(GLOB children "${unpacked}/*")
    list(FILTER children EXCLUDE REGEX "/\\.extracted$")
    list(LENGTH children child_count)
    if(child_count EQUAL 1)
        list(GET children 0 child)
        if(IS_DIRECTORY "${child}")
            set(unpacked "${child}")
        endif()
    endif()
    string(TOUPPER "${name}" name_upper)
    set("FETCHCONTENT_SOURCE_DIR_${name_upper}" "${unpacked}" PARENT_SCOPE)
    message(STATUS "Pikmin 2: local ${name} from ${archive}")
endfunction()
if(IOS)
    # iOS has no installed SDL3: build the pinned AURORA_SDL3_REF from a local archive.
    set(AURORA_SDL3_PROVIDER vendor CACHE STRING "Build SDL3 from source" FORCE)
    p2_local_renderer_dependency(SDL "${CMAKE_CURRENT_SOURCE_DIR}/../references/SDL3-3.4.10.tar.gz"
        "0dc11d980ba17250200718fa4e28011da293f27ed92f92203afffe396811f307")
    # Dawn from source (AURORA_DAWN_REF), not the prebuilt ios-arm64 package: that
    # package targets iOS 14.0, and Dawn compiles BC texture support (the GPV pack
    # is BC7) only when the deployment target is 16.4 or later.
    set(AURORA_DAWN_PROVIDER vendor CACHE STRING "Build Dawn from source" FORCE)
    p2_local_renderer_dependency(dawn "${CMAKE_CURRENT_SOURCE_DIR}/../references/dawn-src-1155e0ed.tar.gz"
        "d0d291936d02a56b3b7e92e84e8b8c71db04a9c9e2b3a41cc6d7540cf13b4167")
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/patch_dawn.py"
        "${FETCHCONTENT_SOURCE_DIR_DAWN}" COMMAND_ERROR_IS_FATAL ANY)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/patch_dawn.py")
    set(P2_DAWN_ARCHIVE "" CACHE FILEPATH "Unused on iOS" FORCE)
else()
    set(P2_DAWN_ARCHIVE "${CMAKE_CURRENT_SOURCE_DIR}/../references/dawn-darwin-arm64.tar.gz"
        CACHE FILEPATH "Local Dawn v20260807.225922 package, when network access is unavailable")
endif()
if(P2_DAWN_ARCHIVE AND EXISTS "${P2_DAWN_ARCHIVE}")
    set(AURORA_DAWN_PROVIDER package CACHE STRING "Use the local Dawn package" FORCE)
    set(AURORA_DAWN_PACKAGE_URL "${P2_DAWN_ARCHIVE}")
    p2_local_renderer_dependency(dawn_prebuilt "${P2_DAWN_ARCHIVE}" "")
    message(STATUS "Pikmin 2: using local Dawn archive ${P2_DAWN_ARCHIVE}")
endif()
p2_local_renderer_dependency(xxhash "${CMAKE_CURRENT_SOURCE_DIR}/../references/xxHash-0.8.3.tar.gz"
    "aae608dfe8213dfd05d909a57718ef82f30722c392344583d3f39050c7f29a80")
p2_local_renderer_dependency(imgui "${CMAKE_CURRENT_SOURCE_DIR}/../references/imgui-1.91.9b-docking.tar.gz"
    "466fdef9b18de15f0bb6e288e3d00ffa3d82200ec458ce5e4f724a161d9528a5")
p2_local_renderer_dependency(tracy "${CMAKE_CURRENT_SOURCE_DIR}/../references/tracy-6789e7d6f9a65ec98926b602097a33a9676d2606.tar.gz"
    "ebfe4fb50d7c254901979355c80a7d4cd33624aa2ec0fe90b3b238153cd5d69b")
add_subdirectory("${P2_RENDERER}" "${CMAKE_CURRENT_BINARY_DIR}/renderer" EXCLUDE_FROM_ALL)
if(IOS)
    # Dawn at -O3 like the prebuilt Release packages (the source build at this
    # project's -O2 measured ~15% slower on the iPad at 1x); game code stays -O2.
    function(p2_dawn_optimize directory)
        get_property(targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
        foreach(target IN LISTS targets)
            get_target_property(type ${target} TYPE)
            if(type MATCHES "^(STATIC_LIBRARY|OBJECT_LIBRARY|SHARED_LIBRARY|EXECUTABLE)$")
                target_compile_options(${target} PRIVATE -O3)
            endif()
        endforeach()
        get_property(children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
        foreach(child IN LISTS children)
            p2_dawn_optimize("${child}")
        endforeach()
    endfunction()
    p2_dawn_optimize("${FETCHCONTENT_SOURCE_DIR_DAWN}")
endif()
# THP movie audio is decoded by Aurora's THPAudio.cpp; only real-audio apps mix it.
set_source_files_properties("${P2_PREPARED}/src/sysGCU/THPDraw.c" PROPERTIES LANGUAGE CXX)
add_library(p2_thp_player STATIC src/thp_player.cpp "${P2_PREPARED}/src/sysGCU/THPDraw.c")
target_link_libraries(p2_thp_player PRIVATE p2_host_flags p2_dvd p2_host_scratch aurora::thp)
target_compile_options(p2_thp_player PRIVATE -Wno-c++11-narrowing)
add_executable(p2_thp_decode_checks tests/thp_decode_checks.cpp)
target_link_libraries(p2_thp_decode_checks PRIVATE aurora::thp p2_assets)
add_test(NAME native_thp_decode COMMAND p2_thp_decode_checks "${P2_DISC_DIR}")
set_tests_properties(native_thp_decode PROPERTIES TIMEOUT 120)
add_executable(p2_thp_player_checks tests/thp_player_checks.cpp)
target_link_libraries(p2_thp_player_checks PRIVATE p2_host_flags p2_thp_player p2_memory)
add_test(NAME native_thp_player COMMAND p2_thp_player_checks "${P2_DISC_DIR}")
set_tests_properties(native_thp_player PROPERTIES TIMEOUT 30)
add_executable(p2_renderer_probe src/renderer_probe.cpp)
target_link_libraries(p2_renderer_probe PRIVATE aurora::gx aurora::gd aurora::os aurora::vi)
target_compile_features(p2_renderer_probe PRIVATE cxx_std_20)
add_library(p2_gx_fifo STATIC src/gx_fifo.cpp src/renderer_gx.cpp)
target_include_directories(p2_gx_fifo PUBLIC include PRIVATE "${P2_RENDERER}/lib")
target_link_libraries(p2_gx_fifo PRIVATE aurora::gx)
target_compile_features(p2_gx_fifo PRIVATE cxx_std_20)
add_executable(p2_gx_fifo_checks tests/gx_fifo_checks.cpp)
target_include_directories(p2_gx_fifo_checks PRIVATE "${P2_RENDERER}/lib")
target_compile_features(p2_gx_fifo_checks PRIVATE cxx_std_20)
target_link_libraries(p2_gx_fifo_checks PRIVATE p2_gx_fifo aurora::gx aurora::os aurora::vi)
add_test(NAME native_gx_fifo_bridge COMMAND p2_gx_fifo_checks)

# Aurora matrix target does not inherit core's fixed-width native SDK types.
target_compile_definitions(aurora_mtx PUBLIC TARGET_PC=1)
add_library(p2_renderer_math STATIC src/renderer_matrix.cpp)
target_link_libraries(p2_renderer_math PUBLIC aurora::mtx)
target_compile_features(p2_renderer_math PRIVATE cxx_std_11)
add_executable(p2_renderer_math_checks tests/renderer_math_checks.cpp)
target_link_libraries(p2_renderer_math_checks PRIVATE p2_host_flags p2_renderer_math p2_math)
add_test(NAME native_renderer_math COMMAND p2_renderer_math_checks)

# One arena owner for game heaps and renderer physical-address translation.
# Replace conflicting SDK implementations without modifying the pinned snapshot.
get_target_property(p2_aurora_os_sources aurora_os SOURCES)
list(FILTER p2_aurora_os_sources EXCLUDE REGEX "/(OSMemory|OSArena|OSAlloc|OSReport)\\.cpp$")
set_property(TARGET aurora_os PROPERTY SOURCES "${p2_aurora_os_sources}")
target_sources(aurora_os PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/renderer_memory.cpp")
target_include_directories(aurora_os PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(aurora_os PRIVATE p2_memory)
add_library(p2_renderer_memory_api STATIC tests/renderer_memory_api.cpp)
target_link_libraries(p2_renderer_memory_api PRIVATE aurora::os aurora::core)
add_executable(p2_renderer_memory_checks tests/renderer_memory_checks.cpp)
target_link_libraries(p2_renderer_memory_checks PRIVATE p2_heap p2_renderer_memory_api aurora::os aurora::gx aurora::vi)
target_link_options(p2_renderer_memory_checks PRIVATE -Wl,-dead_strip)
add_test(NAME native_renderer_memory COMMAND p2_renderer_memory_checks)
add_test(NAME native_renderer_memory_heap_first COMMAND p2_renderer_memory_checks heap-first)

# Retail CARDInit has no parameters; Aurora requires game/maker identifiers.
target_compile_definitions(aurora_card PRIVATE CARDInit=p2_aurora_card_init)
# Input record/replay fingerprints the card directory, so it shares its root.
add_library(p2_card STATIC src/card.cpp src/input_record.cpp)
target_include_directories(p2_card PRIVATE include)
target_link_libraries(p2_card PRIVATE aurora::card aurora::pad "$<LINK_ONLY:p2_heap>" "$<LINK_ONLY:p2_math>")
target_compile_features(p2_card PRIVATE cxx_std_20)
target_compile_definitions(p2_card PRIVATE P2_CARD_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/userdata/cards")
add_executable(p2_card_checks tests/card_checks.cpp)
target_link_libraries(p2_card_checks PRIVATE p2_host_flags p2_card p2_heap aurora::os aurora::gx aurora::vi)
add_test(NAME native_card_storage COMMAND p2_card_checks)

# The native VI owns flushed framebuffer/black state; Aurora supplies mode setup.
target_compile_definitions(aurora_vi PRIVATE VIFlush=p2_aurora_vi_flush)

# Touch overlay: Pikmin 1's on-screen pad (src/touch_pad.cpp), drawn in Aurora's final pass.
add_library(p2_renderer_frame STATIC src/renderer_frame.cpp src/touch_pad.cpp src/touch_draw.cpp)
target_include_directories(p2_renderer_frame PRIVATE include "${P2_RENDERER}/lib")
target_compile_features(p2_renderer_frame PRIVATE cxx_std_20)
target_link_libraries(p2_renderer_frame PRIVATE aurora::gx aurora::os aurora::vi aurora::pad "$<LINK_ONLY:p2_video>" "$<LINK_ONLY:p2_heap>")

target_include_directories(p2_renderer_probe PRIVATE include)
target_link_libraries(p2_renderer_probe PRIVATE p2_renderer_frame)

add_executable(p2_pad_abi_checks tests/pad_abi_checks.cpp)
target_link_libraries(p2_pad_abi_checks PRIVATE p2_host_flags aurora::pad aurora::gx aurora::os aurora::vi)
add_test(NAME native_controller_status COMMAND p2_pad_abi_checks)

target_compile_definitions(p2_renderer_frame PRIVATE P2_RENDERER_LOG_PATH="${P2_LOG_DIR}native-renderer.log")
