# The game on the PC (host.c). From the package's folder: make -C host, then host/host SCRIPT OUTDIR.
SRC = host.c $(addprefix ../main/,cl.c world.c) $(addprefix ../../../engine/,fart.c fx.c ui.c flag.c interior.c viewturn.c)
FONT = ../../../../components/fh1_ui/fh1_font8.c

host: $(SRC) ../main/*.h ../../../engine/*.h font.inc
	cc -DPC_HOST -O1 -g -fstack-protector-all -Werror=missing-field-initializers -Werror=parentheses -Werror=unused-variable -Werror=unused-const-variable -I. -I../main -I../../../engine -I../../../../components/fh1_sdk/include -o $@ $(SRC) -lm

font.inc: $(FONT)
	sed -n '/static const uint8_t glyph_bitmap/,/^};/p' $(FONT) > $@
