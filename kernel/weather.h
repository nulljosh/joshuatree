#ifndef WEATHER_H
#define WEATHER_H
/* kernel/weather.c: the menu-bar weather (Open-Meteo, ip-api or loc=, the particles, WEATHER.TXT). What the rest of
   the kernel reads and calls is here; so is what weather.c borrows from kernel.c (the cmdline overrides, the wind
   level, the menu bar's redraw and the data stamp windows poll). */
#define WX_NONE    0 /* never tried */
#define WX_OK      1
#define WX_OFFLINE 2 /* no NIC, or the gateway never answered ARP */
#define WX_TIMEOUT 3 /* DNS, connect or reply deadline ran out */
#define WX_FAILED  4 /* resolver said no such host, or the NIC refused the frame */
#define WX_BAD     5 /* non-200, empty, or a body the parser could not read */
#define LOC_NAME_MAX 24
#define WIND_HORIZON_ROW 395   /* logical row of the photo's skyline */
#define WIND_TOP_ROW      30   /* just under the menu bar */

/* The rain and snow overlay. Exposed for the shell's self-tests in kernel.c run(). */
#define WEATHER_FX_NONE 0
#define WEATHER_FX_RAIN 1
#define WEATHER_FX_SNOW 2
#define WEATHER_PARTICLE_COUNT 36
struct weather_particle { int x, y, speed; };
extern struct weather_particle weather_particles[WEATHER_PARTICLE_COUNT];
extern int weather_particles_seeded;
int weather_fx_kind(int code);               /* a WMO code to NONE, RAIN or SNOW */
void weather_particles_seed(int w);
void weather_fx_tick_kind(int kind, int w);
int wind_pct_for_weather_code(int code);     /* how hard the wallpaper's grass leans, 0 to 100 */
const char *weather_word(int code);          /* a WMO code as one word: Clear, Rain, Snow */
int json_current_number(const char *json, const char *key, int *out_x10);   /* a number from inside "current":{...}, x10 */

extern char weather_text[24], weather_err[48];
extern unsigned int weather_last_tick;
extern int weather_tried_once, weather_temp_c, weather_code10, weather_have, weather_state;
extern char geo_lat[16], geo_lon[16], geo_city[24];
extern int geo_have;
extern char loc_name[LOC_NAME_MAX], loc_lat[16], loc_lon[16], loc_err[48];
extern int loc_have;
const char *weather_state_name(int st);
extern int weather_hit_x0, weather_hit_x1;   /* the menu-bar weather text's hit box, set by gui_draw_menubar */
void weather_fetch(void);                    /* geo (or loc=) then Open-Meteo; fills the state above and WEATHER.TXT */
void weather_fx_tick(int w);                 /* one frame of rain or snow over the desktop, w physical pixels wide */
int geo_fetch(void);                         /* ip-api: this machine's own lat, lon and city, 1 on success */
int loc_geocode(const char *query);          /* loc=PLACE: Open-Meteo's geocoder, 1 when loc_* are set */
void weather_ring3_open(void);               /* the dock's Weather, the blocking fallback */
int weather_parse_check(int *t10, int *code10);              /* shell self-tests (kernel.c run()) */
void weather_fx_check(int *ok_kind, int *moved, int *still);

/* from kernel.c */
extern char wx_override_host[20];
extern unsigned short wx_override_port;
extern int wind_weather_pct;
extern volatile unsigned jt_data_stamp;
void gui_menubar_force_redraw(void);
#endif
