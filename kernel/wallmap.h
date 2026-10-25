#ifndef WALLMAP_H
#define WALLMAP_H
/* kernel/wallmap.c: the map and satellite wallpapers (tile fetch, theme switch, the baked satellite fallback, apply).
   The themes and the map's geometry are here; so is the desktop state wallmap.c reads and writes, which kernel.c owns
   and its per-pixel readers use. */
#define WALLPAPER_W 960   /* the size of drivers/wallpaper.h's photo, which the map crops to */
#define WALLPAPER_H 540
#define WALL_ZOOM 14
#define WALL_TILE 256   /* OpenTopoMap serves 256px tiles, no @2x variant */
#define WALL_COLS 4     /* 4x3 grid = 1024x768, the smallest that covers a centered 960x540 crop */
#define WALL_ROWS 3
#define WALL_PHOTO 0
#define WALL_WARM  1
#define WALL_COOL  2
#define WALL_RAW   3
#define WALL_SAT   4
/* 4.11.4: plain walls, the same four names and colours as the Pi's Settings
   (arch/arm64/settings.c). Numbered after the old five so a saved wall=0..4
   keeps meaning what it meant. Grey is the default; none of these fetch a
   tile or tint the colour (a plain wall is exactly the colour you picked). */
#define WALL_GREY  5
#define WALL_TERRA 6
#define WALL_CLAY  7
#define WALL_INK   8
#define WALL_COUNT 9
#define WALL_THEME_FETCHES() (wall_theme >= WALL_WARM && wall_theme <= WALL_SAT)

extern char tile_override_host[20];          /* tilehost= on the command line (kmain writes it): fake tile servers in checks */
extern unsigned short tile_override_port;
extern int wall_theme_override;              /* walltheme= for one boot, applied after settings_load */
int wall_fetch(void);                        /* the tiles around geo_lat/geo_lon into wall_map; 1 on success */
void wall_switch_theme(int theme);
void wall_apply(int want_map);               /* point wall_src at the photo, a plain colour, the satellite or the map */

/* from kernel.c: the desktop's wallpaper state */
extern const unsigned char *const wall_photo;   /* drivers/wallpaper.h's baked photo (1.5 MB: only kernel.c includes it) */
extern const unsigned char *wall_src;
extern unsigned char *wall_sat_rgb, *wall_map;
extern const char *const wall_solid_name[4];             /* kernel.c: Grey, Terracotta, Clay, Ink */
extern const unsigned char wall_cycle_order[];          /* kernel.c: the order Settings and `wallpaper` step through */
extern unsigned char wall_solid_row[WALLPAPER_W * 3 * 2];
extern int wall_map_tx, wall_map_ty, wall_map_cx, wall_map_cy, wall_map_is_sat, wall_theme;
extern const unsigned int wall_solid_color[4];
void wall_caches_drop(void);
void settings_save(void);
#endif
