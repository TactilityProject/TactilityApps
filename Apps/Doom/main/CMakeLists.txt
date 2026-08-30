# App sources
file(GLOB_RECURSE APP_SOURCES Source/*.c*)

# doomgeneric engine sources (vendored from https://github.com/ozkl/doomgeneric, minus the platform backends we don't use)
set(DOOMGENERIC_DIR doomgeneric)
set(DOOM_SOURCES
    ${DOOMGENERIC_DIR}/doomgeneric/dummy.c
    ${DOOMGENERIC_DIR}/doomgeneric/am_map.c
    ${DOOMGENERIC_DIR}/doomgeneric/doomdef.c
    ${DOOMGENERIC_DIR}/doomgeneric/doomstat.c
    ${DOOMGENERIC_DIR}/doomgeneric/dstrings.c
    ${DOOMGENERIC_DIR}/doomgeneric/d_event.c
    ${DOOMGENERIC_DIR}/doomgeneric/d_items.c
    ${DOOMGENERIC_DIR}/doomgeneric/d_iwad.c
    ${DOOMGENERIC_DIR}/doomgeneric/d_loop.c
    ${DOOMGENERIC_DIR}/doomgeneric/d_main.c
    ${DOOMGENERIC_DIR}/doomgeneric/d_mode.c
    ${DOOMGENERIC_DIR}/doomgeneric/d_net.c
    ${DOOMGENERIC_DIR}/doomgeneric/f_finale.c
    ${DOOMGENERIC_DIR}/doomgeneric/f_wipe.c
    ${DOOMGENERIC_DIR}/doomgeneric/g_game.c
    ${DOOMGENERIC_DIR}/doomgeneric/hu_lib.c
    ${DOOMGENERIC_DIR}/doomgeneric/hu_stuff.c
    ${DOOMGENERIC_DIR}/doomgeneric/info.c
    ${DOOMGENERIC_DIR}/doomgeneric/i_cdmus.c
    ${DOOMGENERIC_DIR}/doomgeneric/i_endoom.c
    ${DOOMGENERIC_DIR}/doomgeneric/i_joystick.c
    ${DOOMGENERIC_DIR}/doomgeneric/i_scale.c
    ${DOOMGENERIC_DIR}/doomgeneric/i_sound.c
    # OPL3 music: DBOPL (the DOSBox OPL emulator) behind a Nuked-compatible
    # shim, plus the Chocolate Doom GENMIDI player vendored from the Gin_Doom https://github.com/FigBug/Gin_Doom
    # fork (pull-based OPL_Render, no SDL).
    #
    # Nuked was ~2x over the real-time budget on P4 and ymfm ~108%; DBOPL is
    # table-driven rather than cycle-accurate and measures ~19-30x cheaper
    # than ymfm, which is what makes 18-voice OPL3 affordable here.
    ${DOOMGENERIC_DIR}/doomgeneric/opl3.cpp
    ${DOOMGENERIC_DIR}/doomgeneric/dbopl/dbopl.cpp
    ${DOOMGENERIC_DIR}/doomgeneric/opl.c
    ${DOOMGENERIC_DIR}/doomgeneric/opl_queue.c
    ${DOOMGENERIC_DIR}/doomgeneric/i_oplmusic.c
    ${DOOMGENERIC_DIR}/doomgeneric/midifile.c
    ${DOOMGENERIC_DIR}/doomgeneric/mus2mid.c
    ${DOOMGENERIC_DIR}/doomgeneric/i_system.c
    ${DOOMGENERIC_DIR}/doomgeneric/i_timer.c
    ${DOOMGENERIC_DIR}/doomgeneric/memio.c
    ${DOOMGENERIC_DIR}/doomgeneric/m_argv.c
    ${DOOMGENERIC_DIR}/doomgeneric/m_bbox.c
    ${DOOMGENERIC_DIR}/doomgeneric/m_cheat.c
    ${DOOMGENERIC_DIR}/doomgeneric/m_config.c
    ${DOOMGENERIC_DIR}/doomgeneric/m_controls.c
    ${DOOMGENERIC_DIR}/doomgeneric/m_fixed.c
    ${DOOMGENERIC_DIR}/doomgeneric/m_menu.c
    ${DOOMGENERIC_DIR}/doomgeneric/m_misc.c
    ${DOOMGENERIC_DIR}/doomgeneric/m_random.c
    ${DOOMGENERIC_DIR}/doomgeneric/p_ceilng.c
    ${DOOMGENERIC_DIR}/doomgeneric/p_doors.c
    ${DOOMGENERIC_DIR}/doomgeneric/p_enemy.c
    ${DOOMGENERIC_DIR}/doomgeneric/p_floor.c
    ${DOOMGENERIC_DIR}/doomgeneric/p_inter.c
    ${DOOMGENERIC_DIR}/doomgeneric/p_lights.c
    ${DOOMGENERIC_DIR}/doomgeneric/p_map.c
    ${DOOMGENERIC_DIR}/doomgeneric/p_maputl.c
    ${DOOMGENERIC_DIR}/doomgeneric/p_mobj.c
    ${DOOMGENERIC_DIR}/doomgeneric/p_plats.c
    ${DOOMGENERIC_DIR}/doomgeneric/p_pspr.c
    ${DOOMGENERIC_DIR}/doomgeneric/p_saveg.c
    ${DOOMGENERIC_DIR}/doomgeneric/p_setup.c
    ${DOOMGENERIC_DIR}/doomgeneric/p_sight.c
    ${DOOMGENERIC_DIR}/doomgeneric/p_spec.c
    ${DOOMGENERIC_DIR}/doomgeneric/p_switch.c
    ${DOOMGENERIC_DIR}/doomgeneric/p_telept.c
    ${DOOMGENERIC_DIR}/doomgeneric/p_tick.c
    ${DOOMGENERIC_DIR}/doomgeneric/p_user.c
    ${DOOMGENERIC_DIR}/doomgeneric/r_bsp.c
    ${DOOMGENERIC_DIR}/doomgeneric/r_data.c
    ${DOOMGENERIC_DIR}/doomgeneric/r_draw.c
    ${DOOMGENERIC_DIR}/doomgeneric/r_main.c
    ${DOOMGENERIC_DIR}/doomgeneric/r_plane.c
    ${DOOMGENERIC_DIR}/doomgeneric/r_segs.c
    ${DOOMGENERIC_DIR}/doomgeneric/r_sky.c
    ${DOOMGENERIC_DIR}/doomgeneric/r_things.c
    ${DOOMGENERIC_DIR}/doomgeneric/sha1.c
    ${DOOMGENERIC_DIR}/doomgeneric/sounds.c
    ${DOOMGENERIC_DIR}/doomgeneric/statdump.c
    ${DOOMGENERIC_DIR}/doomgeneric/st_lib.c
    ${DOOMGENERIC_DIR}/doomgeneric/st_stuff.c
    ${DOOMGENERIC_DIR}/doomgeneric/s_sound.c
    ${DOOMGENERIC_DIR}/doomgeneric/tables.c
    ${DOOMGENERIC_DIR}/doomgeneric/v_video.c
    ${DOOMGENERIC_DIR}/doomgeneric/wi_stuff.c
    ${DOOMGENERIC_DIR}/doomgeneric/w_checksum.c
    ${DOOMGENERIC_DIR}/doomgeneric/w_file.c
    ${DOOMGENERIC_DIR}/doomgeneric/w_main.c
    ${DOOMGENERIC_DIR}/doomgeneric/w_wad.c
    ${DOOMGENERIC_DIR}/doomgeneric/z_zone.c
    ${DOOMGENERIC_DIR}/doomgeneric/w_file_stdc.c
    ${DOOMGENERIC_DIR}/doomgeneric/i_input.c
    ${DOOMGENERIC_DIR}/doomgeneric/i_video.c
    ${DOOMGENERIC_DIR}/doomgeneric/doomgeneric.c
    ${DOOMGENERIC_DIR}/doomgeneric_tactility.c
)

idf_component_register(
    SRCS ${APP_SOURCES} ${DOOM_SOURCES}
    INCLUDE_DIRS "Source" "${DOOMGENERIC_DIR}/doomgeneric"
    REQUIRES TactilitySDK driver esp_driver_ppa esp_mm
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    DOOMGENERIC_RESX=320
    DOOMGENERIC_RESY=200
    FEATURE_SOUND
    # Compiles DBOPL without its DOSBox mixer glue (Adlib::Handler, MixerChannel,
    # iostream save/load), leaving just DBOPL::Chip, which is all the shim uses.
    DBOPL_STANDALONE
)

# Suppress warnings in the vendored doomgeneric engine sources
# Several of these warning flags (-Wno-old-style-declaration, -Wno-int-conversion,
# -Wno-discarded-qualifiers, -Wno-implicit-function-declaration,
# -Wno-incompatible-pointer-types) are C-only; GCC rejects them outright when
# compiling C++. Apply them to the vendored C sources only, and give the C++
# sources (the OPL3 shim and DBOPL itself) their own, milder set.
set(DOOM_C_SOURCES ${DOOM_SOURCES})
list(FILTER DOOM_C_SOURCES EXCLUDE REGEX "\\.cpp$")
set(DOOM_CXX_SOURCES ${DOOM_SOURCES})
list(FILTER DOOM_CXX_SOURCES INCLUDE REGEX "\\.cpp$")

set_source_files_properties(${DOOM_C_SOURCES} PROPERTIES COMPILE_FLAGS
    "-Wno-error -Wno-dangling-pointer -Wno-unused-but-set-parameter -Wno-int-conversion -Wno-implicit-function-declaration -Wno-discarded-qualifiers -Wno-unused-variable -Wno-unused-but-set-variable -Wno-maybe-uninitialized -Wno-format -Wno-misleading-indentation -Wno-old-style-declaration -Wno-incompatible-pointer-types"
)

set_source_files_properties(${DOOM_CXX_SOURCES} PROPERTIES COMPILE_FLAGS
    "-Wno-error -Wno-unused-variable -Wno-unused-but-set-variable -Wno-maybe-uninitialized -Wno-format -Wno-misleading-indentation"
)
