# Diagnostic link of the original game entry point. This target is deliberately
# excluded from default builds. Its native platform services and muted startup
# path link; gameplay remains outside the verified title bring-up scope.
set(P2_LINK_SOURCES ${P2_ENGINE_SOURCES})
list(FILTER P2_LINK_SOURCES EXCLUDE REGEX "/sysBootupU/sysBootup.cpp$")
# Native archive/ARAM replacements cover these old console implementations.
list(FILTER P2_LINK_SOURCES EXCLUDE REGEX "/JKernel/(JKRAramStream|JKRCompArchive|JKRDvdArchive).cpp$")
# The US boot uses JUTResFont; Japanese cache paging is not ported yet.
list(FILTER P2_LINK_SOURCES EXCLUDE REGEX "/JUtility/JUTCacheFont.cpp$")
# Audio is a separate library: apps link real JAudio (p2_audio_real); the link
# audit and checks link the silent sound system, where JAudio's object/table
# bookkeeping is original code and playback below JAIBasic is src/audio_silent.cpp.
set(P2_AUDIO_SILENT_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/audio_silent.cpp")
foreach(p2_jaudio IN ITEMS JAI/JAIObject JAI/JAIAnimation JAI/JAISoundTable JAI/JAISound JAI/JAIGlobalParameter JAI/JAIDummyObject
        JAL/JALCalc JAD/JADHioNode JAU/JAUData JAU/JAUDataMgr JAS/JASInstRand JAS/JASInstEffect JAS/JASOuterParam
        JAS/JASRegisterParam JAS/JASChannelUpdater)
    list(APPEND P2_AUDIO_SILENT_SOURCES "${P2_PREPARED}/src/JSystem/JAudio/${p2_jaudio}.cpp")
endforeach()
add_library(p2_audio_silent STATIC EXCLUDE_FROM_ALL ${P2_AUDIO_SILENT_SOURCES})
target_link_libraries(p2_audio_silent PRIVATE p2_host_flags)
set_target_properties(p2_audio_silent PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED YES)
target_compile_options(p2_audio_silent PRIVATE -ffunction-sections -fdata-sections)
add_library(p2_game_code STATIC EXCLUDE_FROM_ALL ${P2_LINK_SOURCES})
target_link_libraries(p2_game_code PRIVATE p2_host_flags)
set_target_properties(p2_game_code PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED YES)
target_compile_options(p2_game_code PRIVATE -ffunction-sections -fdata-sections)
# Real audio bring-up: every JAudio translation unit, compiled with the game's
# flags; linked by the apps. The link audit and checks keep the silent system.
file(GLOB_RECURSE P2_JAUDIO_SOURCES CONFIGURE_DEPENDS "${P2_PREPARED}/src/JSystem/JAudio/*.cpp")
file(GLOB P2_JSTUDIO_JAUDIO_SOURCES CONFIGURE_DEPENDS "${P2_PREPARED}/src/JSystem/JStudio_JAudio/*.cpp")
list(APPEND P2_JAUDIO_SOURCES ${P2_JSTUDIO_JAUDIO_SOURCES})
add_library(p2_audio_real STATIC EXCLUDE_FROM_ALL ${P2_JAUDIO_SOURCES})
target_link_libraries(p2_audio_real PRIVATE p2_host_flags)
set_target_properties(p2_audio_real PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED YES)
target_compile_options(p2_audio_real PRIVATE -ffunction-sections -fdata-sections)
# Host output and voice mixer that replace AI DMA and the DSP (src/audio_host.cpp).
add_library(p2_freeverb STATIC EXCLUDE_FROM_ALL third_party/freeverb/comb.cpp third_party/freeverb/allpass.cpp
    third_party/freeverb/revmodel.cpp)
target_include_directories(p2_freeverb PRIVATE third_party/freeverb/include/freeverb INTERFACE third_party/freeverb/include)
add_library(p2_audio_host STATIC EXCLUDE_FROM_ALL src/audio_host.cpp)
target_link_libraries(p2_audio_host PRIVATE p2_host_flags ${AURORA_SDL3_TARGET} p2_aram p2_freeverb)
target_include_directories(p2_audio_host PRIVATE include)
set_target_properties(p2_audio_host PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES)
target_link_libraries(p2_audio_real PUBLIC p2_audio_host)
# Game code is compiled with its audio calls in. Apps link real audio; the link
# audit and checks link the silent sound system and never start audio.
target_compile_definitions(p2_game_code PRIVATE P2_AUDIO_ENABLED=1)
target_compile_definitions(p2_audio_real PRIVATE P2_AUDIO_ENABLED=1)
add_library(p2_game_entry OBJECT EXCLUDE_FROM_ALL "${P2_PREPARED}/src/sysBootupU/sysBootup.cpp")
target_link_libraries(p2_game_entry PRIVATE p2_host_flags)
target_compile_definitions(p2_game_entry PRIVATE main=p2_game_main)
add_executable(p2_game_link_audit EXCLUDE_FROM_ALL src/game_main.cpp $<TARGET_OBJECTS:p2_game_entry>)
target_compile_definitions(p2_game_link_audit PRIVATE P2_DISC_ROOT="${P2_DISC_DIR}")
target_link_libraries(p2_game_link_audit PRIVATE p2_host_flags p2_game_code p2_audio_silent
    p2_dvd_worker p2_resource_cache p2_font p2_animation p2_material2d p2_layout_data
    p2_model_data p2_texture_refs p2_particle_data p2_gx_fifo p2_renderer_math p2_card p2_gx_math p2_video p2_renderer_frame p2_platform p2_reset
    p2_thp_player aurora::gx aurora::gd aurora::vi aurora::os aurora::pad)
target_link_options(p2_game_link_audit PRIVATE -Wl,-dead_strip
    "-Wl,-map,${CMAKE_CURRENT_BINARY_DIR}/game-link-audit.map")

# Bring-up app variants. Rendering, assets and game logic are the original
# game; each variant only rejects transitions into modes it does not cover, so
# the full-game audit above remains available and unchanged.
function(p2_bringup_app target bundle_name identifier)
    set(blocked ${ARGN})
    file(READ "${P2_PREPARED}/src/sysGCU/gameflow.cpp" flow)
    foreach(constructor IN LISTS blocked)
        set(original "section = new ${constructor};")
        string(FIND "${flow}" "${original}" constructor_pos)
        if(constructor_pos LESS 0)
            message(FATAL_ERROR "${target} GameFlow patch drift: ${constructor}")
        endif()
        string(REPLACE "${original}"
            "OSPanic(__FILE__, __LINE__, \"This game mode is not available in the ${target} target\"); section = nullptr;"
            flow "${flow}")
    endforeach()
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${target}-gameflow.cpp" CONTENT "${flow}")
    if(IOS)
        # iOS: the game is a library; the Xcode shell (ios/) supplies main() and signing.
        add_library(${target} STATIC EXCLUDE_FROM_ALL src/game_main.cpp src/ios_main.cpp
            "${CMAKE_CURRENT_BINARY_DIR}/${target}-gameflow.cpp" $<TARGET_OBJECTS:p2_game_entry>)
        set_source_files_properties(src/game_main.cpp TARGET_DIRECTORY ${target}
            PROPERTIES COMPILE_DEFINITIONS main=p2_host_main)
        target_link_libraries(${target} PRIVATE ${AURORA_SDL3_TARGET})
        target_compile_definitions(${target} PRIVATE P2_DISC_ROOT="disc")
    else()
        add_executable(${target} EXCLUDE_FROM_ALL src/game_main.cpp
            "${CMAKE_CURRENT_BINARY_DIR}/${target}-gameflow.cpp" $<TARGET_OBJECTS:p2_game_entry>)
        # Product identity. Empty keeps this tree's bring-up names; the
        # published tree sets them so nothing built presents itself as a
        # Nintendo product. See docs/LEGAL.md in the published tree.
        set(app_name "${bundle_name}")
        set(app_id "${identifier}")
        # Only the one designated target is renamed. Renaming them all makes
        # every bundle land on the same path, and ninja rejects the duplicate.
        if(P2_APP_TARGET STREQUAL "${target}")
            if(P2_APP_NAME)
                set(app_name "${P2_APP_NAME}")
                set_target_properties(${target} PROPERTIES OUTPUT_NAME "${P2_APP_NAME}")
            endif()
            if(P2_APP_ID)
                set(app_id "${P2_APP_ID}")
            endif()
        endif()
        set_target_properties(${target} PROPERTIES MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_GUI_IDENTIFIER "${app_id}"
            MACOSX_BUNDLE_BUNDLE_NAME "${app_name}"
            MACOSX_BUNDLE_BUNDLE_VERSION "1"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "0.1")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND /usr/bin/codesign --force --sign - "$<TARGET_BUNDLE_DIR:${target}>"
            COMMENT "Ad-hoc sign ${target}")
        target_compile_definitions(${target} PRIVATE P2_DISC_ROOT="${P2_APP_DISC_ROOT}")
        target_link_options(${target} PRIVATE -Wl,-dead_strip "-Wl,-map,${CMAKE_CURRENT_BINARY_DIR}/${target}.map")
    endif()
    get_target_property(game_link_libraries p2_game_link_audit LINK_LIBRARIES)
    list(TRANSFORM game_link_libraries REPLACE "^p2_audio_silent$" "p2_audio_real")
    target_link_libraries(${target} PRIVATE ${game_link_libraries})
endfunction()

# Title regression baseline: every gameplay mode is rejected.
p2_bringup_app(p2_title_bringup "Pikmin 2 Metal" local.pikmin2.metal.title
    "Game::SingleGameSection(heap)" "Game::VsGameSection(heap, false)" "Game::VsGameSection(heap, true)")
# Gameplay bring-up: Story, Challenge and 2-Player Battle are all reachable.
p2_bringup_app(p2_story_bringup "Pikmin 2 Metal Story" local.pikmin2.metal.story)

if(IOS)
    # The same link the Xcode shell performs, so a missing symbol fails here.
    # tools/ios_link_config.py turns its link line into <shell>.xcconfig, which
    # ios/<shell>.xcodeproj includes: every archive and framework, in CMake's order.
    add_executable(p2_ios_link_check EXCLUDE_FROM_ALL "ios/${P2_IOS_SHELL_NAME}/main.m")
    target_link_libraries(p2_ios_link_check PRIVATE p2_story_bringup)
    target_link_options(p2_ios_link_check PRIVATE -Wl,-dead_strip)
    add_custom_command(TARGET p2_ios_link_check POST_BUILD
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/ios_link_config.py"
            --link-txt "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/p2_ios_link_check.dir/link.txt"
            --build-dir "${CMAKE_CURRENT_BINARY_DIR}" --out "${CMAKE_CURRENT_BINARY_DIR}/${P2_IOS_SHELL_NAME}.xcconfig"
        COMMENT "Write ${P2_IOS_SHELL_NAME}.xcconfig for the Xcode shell")
endif()

get_target_property(p2_game_link_libs p2_game_link_audit LINK_LIBRARIES)
# Exercise the boot configuration loaders without creating a desktop window.
add_executable(p2_movie_config_checks EXCLUDE_FROM_ALL tests/movie_config_checks.cpp)
target_link_libraries(p2_movie_config_checks PRIVATE ${p2_game_link_libs})
target_link_options(p2_movie_config_checks PRIVATE -Wl,-dead_strip)

# Construct the actual title hierarchy, including picture/window records.
add_executable(p2_title_layout_checks EXCLUDE_FROM_ALL tests/title_layout_checks.cpp)
target_link_libraries(p2_title_layout_checks PRIVATE ${p2_game_link_libs} p2_assets)
target_link_options(p2_title_layout_checks PRIVATE -Wl,-dead_strip)

add_executable(p2_model_matrix_checks EXCLUDE_FROM_ALL tests/model_matrix_checks.cpp)
target_link_libraries(p2_model_matrix_checks PRIVATE ${p2_game_link_libs})
target_link_options(p2_model_matrix_checks PRIVATE -Wl,-dead_strip)
