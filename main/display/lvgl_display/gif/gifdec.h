#ifndef GIFDEC_H
#define GIFDEC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <lvgl.h>

#include <stdint.h>

// Compile-time switch: 1=use RGB565 canvas (2B/px), 0=use ARGB8888 canvas (4B/px)
#ifndef GIFDEC_USE_RGB565
#define GIFDEC_USE_RGB565 1
#endif

// Enable LZW decode working cache to improve performance (adds ~16KB per decoder instance)
#ifndef LV_GIF_CACHE_DECODE_DATA
#define LV_GIF_CACHE_DECODE_DATA 1
#endif

/* Prefetch compressed sub-blocks into RAM and decode from memory for speed */
#ifndef LV_GIF_PREFETCH_SUBBLOCKS
#define LV_GIF_PREFETCH_SUBBLOCKS 1
#endif

typedef struct _gd_Palette {
    int size;
    uint8_t colors[0x100 * 3];
} gd_Palette;

typedef struct _gd_GCE {
    uint16_t delay;
    uint8_t tindex;
    uint8_t disposal;
    int input;
    int transparency;
} gd_GCE;



typedef struct _gd_GIF {
    lv_fs_file_t fd;
    const char * data;
    uint8_t is_file;
    uint32_t f_rw_p;
    int32_t anim_start;
    uint16_t width, height;
    uint16_t depth;
    int32_t loop_count;
    gd_GCE gce;
    gd_Palette * palette;
    gd_Palette lct, gct;
    void (*plain_text)(
        struct _gd_GIF * gif, uint16_t tx, uint16_t ty,
        uint16_t tw, uint16_t th, uint8_t cw, uint8_t ch,
        uint8_t fg, uint8_t bg
    );
    void (*comment)(struct _gd_GIF * gif);
    void (*application)(struct _gd_GIF * gif, char id[8], char auth[3]);
    uint16_t fx, fy, fw, fh;
    uint8_t bgindex;
    uint8_t * canvas, * frame;
#if LV_GIF_CACHE_DECODE_DATA
    uint8_t *lzw_cache;
#endif
#if GIFDEC_USE_RGB565
    uint16_t pal16_cache[256];
    uint8_t  pal_dirty; /* 1 if palette changed and cache needs rebuild */
#endif
} gd_GIF;

gd_GIF * gd_open_gif_file(const char * fname);

gd_GIF * gd_open_gif_data(const void * data);

void gd_render_frame(gd_GIF * gif, uint8_t * buffer);

int gd_get_frame(gd_GIF * gif);
void gd_rewind(gd_GIF * gif);
void gd_close_gif(gd_GIF * gif);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* GIFDEC_H */
