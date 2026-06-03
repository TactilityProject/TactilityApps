/*******************************************************************************
 * Size: 20 px
 * Bpp: 2
 * Opts: --no-compress --no-prefilter --bpp 2 --size 20 --font MaterialSymbolsRounded.ttf -r 0xF53E,0xF1C6 --format lvgl -o ..\source-fonts\material_symbols_shared_20.c --force-fast-kern-format
 ******************************************************************************/

#include "lvgl.h"

#ifndef MATERIAL_SYMBOLS_SHARED_20
#define MATERIAL_SYMBOLS_SHARED_20 1
#endif

#if MATERIAL_SYMBOLS_SHARED_20

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+F1C6 "" */
    0x1a, 0xaa, 0xa4, 0x0, 0x7f, 0xff, 0xfe, 0x0,
    0xb0, 0x0, 0xb, 0x80, 0xb0, 0x15, 0x42, 0xe0,
    0xb0, 0xff, 0xe0, 0xb8, 0xb0, 0x55, 0x40, 0x2d,
    0xb0, 0x0, 0x0, 0xe, 0xb0, 0xff, 0xff, 0xe,
    0xb0, 0xff, 0xff, 0xe, 0xb0, 0x0, 0x0, 0xe,
    0xb0, 0x55, 0x55, 0xe, 0xb0, 0xff, 0xff, 0xe,
    0xb0, 0x15, 0x54, 0xe, 0xb0, 0x0, 0x0, 0xe,
    0x7f, 0xff, 0xff, 0xfd, 0x1a, 0xaa, 0xaa, 0xa4,

    /* U+F53E "" */
    0x6, 0xaa, 0xaa, 0x43, 0xff, 0xff, 0xfe, 0x74,
    0xe0, 0x0, 0xeb, 0xe, 0x0, 0xe, 0xb0, 0xe0,
    0x0, 0xeb, 0xe, 0x0, 0xe, 0xb0, 0xe0, 0x0,
    0xeb, 0xe, 0x0, 0xe, 0xb0, 0xe0, 0x0, 0xeb,
    0xe, 0x0, 0xe, 0xb0, 0xe0, 0x0, 0xeb, 0x6f,
    0xaa, 0xae, 0xbf, 0xff, 0xff, 0xeb, 0x40, 0x0,
    0x3c, 0xb0, 0x0, 0x3, 0x87, 0xea, 0xaa, 0xbd,
    0x1f, 0xff, 0xff, 0xe0, 0x0, 0x0, 0x0
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 320, .box_w = 16, .box_h = 16, .ofs_x = 2, .ofs_y = 2},
    {.bitmap_index = 64, .adv_w = 320, .box_w = 14, .box_h = 18, .ofs_x = 3, .ofs_y = 1}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/

static const uint16_t unicode_list_0[] = {
    0x0, 0x378
};

/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 61894, .range_length = 889, .glyph_id_start = 1,
        .unicode_list = unicode_list_0, .glyph_id_ofs_list = NULL, .list_length = 2, .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY
    }
};



/*--------------------
 *  ALL CUSTOM DATA
 *--------------------*/

#if LVGL_VERSION_MAJOR == 8
/*Store all the custom data of the font*/
static  lv_font_fmt_txt_glyph_cache_t cache;
#endif

#if LVGL_VERSION_MAJOR >= 8
static const lv_font_fmt_txt_dsc_t font_dsc = {
#else
static lv_font_fmt_txt_dsc_t font_dsc = {
#endif
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = 1,
    .bpp = 2,
    .kern_classes = 0,
    .bitmap_format = 0,
#if LVGL_VERSION_MAJOR == 8
    .cache = &cache
#endif
};



/*-----------------
 *  PUBLIC FONT
 *----------------*/

/*Initialize a public general font descriptor*/
#if LVGL_VERSION_MAJOR >= 8
const lv_font_t material_symbols_shared_20 = {
#else
lv_font_t material_symbols_shared_20 = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 18,          /*The maximum line height required by the font*/
    .base_line = -1,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = 0,
    .underline_thickness = 0,
#endif
    .dsc = &font_dsc,          /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9
    .fallback = NULL,
#endif
    .user_data = NULL,
};



#endif /*#if MATERIAL_SYMBOLS_SHARED_20*/

