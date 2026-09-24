/*******************************************************************************
 * Size: 44 px
 * Bpp: 4
 * Opts: --font C:/Windows/Fonts/seguisym.ttf -r 0x2665 --size 44 --bpp 4 --format lvgl --force-fast-kern-format --lv-font-name lv_font_heart_44 -o E:\music_nano\music_nano\main\resources\lv_font_heart_44.c
 ******************************************************************************/

#ifdef __has_include
    #if __has_include("lvgl.h")
        #ifndef LV_LVGL_H_INCLUDE_SIMPLE
            #define LV_LVGL_H_INCLUDE_SIMPLE
        #endif
    #endif
#endif

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
    #include "lvgl.h"
#else
    #include "lvgl/lvgl.h"
#endif



#ifndef LV_FONT_HEART_44
#define LV_FONT_HEART_44 1
#endif

#if LV_FONT_HEART_44

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+2665 "♥" */
    0x00, 0xe2, 0x32, 0x00, 0xff, 0x11, 0x90, 0x07,
    0xf3, 0xf6, 0xe6, 0xfc, 0x88, 0x06, 0x18, 0xfd,
    0xcd, 0xe8, 0x00, 0xf6, 0x40, 0x80, 0x66, 0xe3,
    0x00, 0x17, 0xb8, 0x06, 0x17, 0xd1, 0x00, 0xa8,
    0xc0, 0x3f, 0x61, 0x0e, 0x08, 0x07, 0xc5, 0xa0,
    0x03, 0x50, 0x0f, 0xf7, 0xe8, 0x80, 0x7f, 0x1a,
    0x85, 0x00, 0x7f, 0xc4, 0x60, 0x1f, 0xf7, 0x02,
    0x00, 0x7f, 0xf3, 0x10, 0x0c, 0x03, 0xff, 0x9c,
    0x20, 0x1f, 0xfc, 0xd1, 0x10, 0x07, 0xff, 0x34,
    0x48, 0x03, 0xff, 0x98, 0x20, 0x80, 0x1f, 0xfc,
    0xc4, 0x0b, 0x00, 0xff, 0xe6, 0x78, 0x19, 0x80,
    0x3f, 0xf9, 0x42, 0xa0, 0x08, 0x00, 0xff, 0xe5,
    0x40, 0x04, 0x8a, 0x01, 0xff, 0xc8, 0x36, 0x00,
    0xd6, 0x20, 0x1f, 0xfc, 0x7e, 0x00, 0xe1, 0xd0,
    0x0f, 0xfe, 0x35, 0x10, 0x07, 0x8e, 0x80, 0x3f,
    0xf8, 0x92, 0xa0, 0x1f, 0x96, 0x80, 0x3f, 0xf8,
    0x52, 0xc0, 0x1f, 0xe5, 0xb0, 0x0f, 0xfe, 0x04,
    0xb0, 0x07, 0xff, 0x01, 0x2c, 0x03, 0xfd, 0x4c,
    0x01, 0xff, 0xc2, 0x4c, 0x10, 0x0f, 0xad, 0x40,
    0x3f, 0xf8, 0x87, 0x84, 0x01, 0x87, 0x10, 0x03,
    0xff, 0x8c, 0x58, 0x40, 0x01, 0xc3, 0x00, 0xff,
    0xe4, 0x0e, 0x10, 0xf9, 0x00, 0x7f, 0xf2, 0x87,
    0x30, 0x20, 0x1f, 0xfc, 0x00
};

/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 482, .box_w = 27, .box_h = 27, .ofs_x = 2, .ofs_y = 2}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/



/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 9829, .range_length = 1, .glyph_id_start = 1,
        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY
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
    .bpp = 4,
    .kern_classes = 0,
    .bitmap_format = 1,
#if LVGL_VERSION_MAJOR == 8
    .cache = &cache
#endif

};



/*-----------------
 *  PUBLIC FONT
 *----------------*/

/*Initialize a public general font descriptor*/
#if LVGL_VERSION_MAJOR >= 8
const lv_font_t lv_font_heart_44 = {
#else
lv_font_t lv_font_heart_44 = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 27,          /*The maximum line height required by the font*/
    .base_line = -2,             /*Baseline measured from the bottom of the line*/
#if LV_VERSION_CHECK(9, 6, 0) || LVGL_VERSION_MAJOR >= 10
    .cap_height = 31,           /*Cap height of the font*/
    .x_height = 22,               /*x-height of the font*/
#endif
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = -4,
    .underline_thickness = 3,
#endif

#if LV_VERSION_CHECK(9, 3, 0)
    .static_bitmap = 0,    /*Bitmaps are stored as const so they are always static if not compressed */
#endif

    .dsc = &font_dsc,          /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9
    .fallback = NULL,
#endif
    .user_data = NULL,
};



#endif /*#if LV_FONT_HEART_44*/
