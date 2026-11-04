cmake_minimum_required(VERSION 3.11)

# Default: PSP EBOOT. Configure with -DDD_HOST=ON for the PC test build (software renderer).
option(DD_HOST "Build the PC test version instead of the PSP EBOOT" OFF)
if(NOT DD_HOST)
    # IDEs started outside a login shell (e.g. CLion from Toolbox) don't see ~/.bashrc,
    # so provide the PSPDEV location the toolchain file requires.
    if(NOT DEFINED ENV{PSPDEV})
        set(ENV{PSPDEV} "$ENV{HOME}/pspdev")
        set(ENV{PATH} "$ENV{HOME}/pspdev/bin:$ENV{PATH}")
    endif()
    set(CMAKE_TOOLCHAIN_FILE "$ENV{PSPDEV}/psp/share/pspdev.cmake")
endif()

project(darkest C)
set(CMAKE_C_STANDARD 99)

# Generated from the user's own game copy by tools/export_game_data.py; never committed.
set(DD_GEN_DIR "${CMAKE_SOURCE_DIR}/../extracted/gen" CACHE PATH "Generated game data")
set(DD_ASSET_DIR "${CMAKE_SOURCE_DIR}/../extracted/assets" CACHE PATH "Converted assets")

set(CORE_SOURCES
        src/core/spine.c
        src/core/assets.c
        src/core/text.c
        src/core/ui.c
        src/core/hud.c
        src/core/battle.c
        src/core/formulas.c
        src/core/effects.c
        src/core/ai.c
        src/core/stress.c
        src/core/dungeon.c
        src/core/campaign.c
        src/core/curio.c
        src/core/loot.c
        src/core/quirk.c
        src/core/trinket.c
        src/core/town_event.c
        src/core/pack.c
        src/core/audio.c
        src/core/game.c
        src/core/scene_title.c
        src/core/scene_battle.c
        src/core/scene_dungeon.c
        src/core/scene_town.c
        src/core/dd_load_gen.c
)

if(DD_HOST)
    add_executable(darkest_host ${CORE_SOURCES} src/platform/host/gfx_soft.c src/platform/host/audio_host.c src/platform/host/main_host.c)
    target_include_directories(darkest_host PRIVATE src/core)
    target_compile_options(darkest_host PRIVATE -Wall -Wextra -g)
    target_link_libraries(darkest_host PRIVATE z m)

    add_executable(battle_sim src/core/battle.c src/core/formulas.c src/core/effects.c src/core/ai.c
            src/core/stress.c src/core/dungeon.c src/core/loot.c src/core/quirk.c src/core/trinket.c src/core/dd_load_gen.c src/platform/host/battle_sim.c)
    target_include_directories(battle_sim PRIVATE src/core)
    target_compile_options(battle_sim PRIVATE -Wall -Wextra -g)
    target_link_libraries(battle_sim PRIVATE m)
else()
    add_executable(darkest ${CORE_SOURCES} src/platform/psp/gfx_psp.c src/platform/psp/audio_psp.c src/platform/psp/main_psp.c)
    target_include_directories(darkest PRIVATE src/core)
    target_compile_options(darkest PRIVATE -O2 -Wall -G0)
    target_compile_definitions(darkest PRIVATE DD_MONSTER_FLIP=1)  # see scene_battle.c draw_actor_ex
    target_link_libraries(darkest PRIVATE z pspgu pspctrl psprtc psppower pspdisplay pspge pspdebug pspaudio vorbisfile vorbis ogg m)
    # XMB art from tools/make_xmb.py (made from the user's own game copy, kept in extracted/)
    set(DD_XMB ${CMAKE_CURRENT_SOURCE_DIR}/../extracted/xmb CACHE PATH "XMB art folder (ICON0.PNG, PIC1.PNG); point elsewhere for a build without art")
    set(DD_ICON0 NULL)
    set(DD_PIC1 NULL)
    if(EXISTS ${DD_XMB}/ICON0.PNG)
        set(DD_ICON0 ${DD_XMB}/ICON0.PNG)
    endif()
    if(EXISTS ${DD_XMB}/PIC1.PNG)
        set(DD_PIC1 ${DD_XMB}/PIC1.PNG)
    endif()
    create_pbp_file(
            TARGET darkest
            ICON_PATH ${DD_ICON0}
            BACKGROUND_PATH ${DD_PIC1}
            PREVIEW_PATH NULL
            TITLE "Darkest Dungeon Demake"
            VERSION 01.00
    )
    # The EBOOT reads assets/ next to itself; link them into the build folder for convenience.
    add_custom_command(TARGET darkest POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E create_symlink ${DD_ASSET_DIR} ${CMAKE_BINARY_DIR}/assets)
endif()
