# The game's own sources, plus the engine shared by the packaged games (firmware/apps/engine: art packs, the
# in-game interface, effects).
set(ENGINE ../../../engine)
idf_component_register(SRCS "taticas.c" "battle.c" "snd.c" "screens.c" "summary.c" "mapgen.c" "data.c" "iso.c"
                            "${ENGINE}/fart.c" "${ENGINE}/fx.c" "${ENGINE}/ui.c" "${ENGINE}/portal.c"
                       INCLUDE_DIRS "." "${ENGINE}"
                       REQUIRES fh1_sdk)
