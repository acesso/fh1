// fh1_sym_8: the FH1's own symbols, U+E000.., 8 x 8 at 1 bit per pixel like fh1_font_8, whose fallback this is
// (so any text in the interface font can show them). Drawn by hand; the macros are in fh1_ui.h (FH1_SYM_*).
#include "lvgl.h"

static const uint8_t glyph_bitmap[] = {
    /* U+E000 bridge */ 0x00, 0x3c, 0x42, 0xff, 0xa5, 0xa5, 0x00, 0x00,
    /* U+E001 gamepad */ 0x00, 0x7e, 0xdd, 0x8b, 0xdf, 0xff, 0xc3, 0x00,
    /* U+E002 Wi-Fi */ 0x00, 0x3c, 0x42, 0x99, 0x24, 0x00, 0x18, 0x18,
    /* U+E003 Bluetooth */ 0x10, 0x18, 0x54, 0x38, 0x38, 0x54, 0x18, 0x10,
};

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0},  /* id 0 reserved */
    {.bitmap_index = 0, .adv_w = 144, .box_w = 8, .box_h = 8, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 8, .adv_w = 144, .box_w = 8, .box_h = 8, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 16, .adv_w = 144, .box_w = 8, .box_h = 8, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 24, .adv_w = 144, .box_w = 8, .box_h = 8, .ofs_x = 0, .ofs_y = -1},
};

static const lv_font_fmt_txt_cmap_t cmaps[] = {
    {.range_start = 0xE000, .range_length = 4, .glyph_id_start = 1, .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY},
};

static const lv_font_fmt_txt_dsc_t font_dsc = {
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = 1,
    .bpp = 1,
    .kern_classes = 0,
    .bitmap_format = 0,
};

const lv_font_t fh1_sym_8 = {
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,
    .line_height = 9,
    .base_line = 1,
    .subpx = LV_FONT_SUBPX_NONE,
    .underline_position = 0,
    .underline_thickness = 0,
    .static_bitmap = 1,
    .dsc = &font_dsc,
    .fallback = NULL,
    .user_data = NULL,
};
