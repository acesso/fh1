# The game's own sources, plus the engine shared by the packaged games (firmware/apps/engine).
set(ENGINE ../../../engine)
idf_component_register(SRCS "cl.c" "world.c" "${ENGINE}/fart.c" "${ENGINE}/fx.c" "${ENGINE}/flag.c" "${ENGINE}/interior.c" "${ENGINE}/viewturn.c" "${ENGINE}/ui.c"
                       INCLUDE_DIRS "." "${ENGINE}"
                       REQUIRES fh1_sdk)
