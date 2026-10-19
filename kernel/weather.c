/* The menu-bar weather: Open-Meteo through the kernel http client, the location from ip-api (or loc= / a geocoded
 place name), the rain and snow particles over the desktop, and the file user/weather.c (ring 3) reads. Moved out of
 kernel.c in 4.12.3; kernel/weather.h is what the rest of the kernel calls. */
#include "libc.h"
#include "net.h"
#include "http.h"
#include "vbe.h"
#include "window.h"
#include "gui_prims.h"
#include "serial.h"
#include "vfs.h"
#include "fat.h"
#include "ramfs.h"
#include "json.h"
#include "irq.h"
#include "ring3app.h"
#include "weather.h"

/* Test/diagnostic override, read once from the multiboot command line
   (`-append "wxhost=10.0.2.2:8099"`, see kmain): both the location and the
   forecast request go to this literal IP:port instead of ip-api.com and
   api.open-meteo.com. tools/checks/weather-app-check.sh points it at a
   local fake server to drive success, bad-response and timeout without
   the real internet. Empty (every normal boot) means the real hosts. */
char wx_override_host[20] = "";
unsigned short wx_override_port = 80;

/* The state the menu bar, the Weather window and SYS_SYSINFO read (declared in weather.h). */
/* v53: real fields behind the one-line summary, kept for the dropdown
   panel. Nothing fabricated: temp/code are exactly what weather_fetch
   already parses out of the reply; lat/lon (v71) are the exact text
   ip-api.com returned for this machine's own public IP (geo_fetch below),
   the same text the http_get() URL is built from, not a literal. */
/* Why the last fetch ended the way it did. The Weather window used to have
   exactly one failure face ("Weather unavailable") for five different
   causes, and no way to try again short of waiting ten minutes.
   weather_have/weather_text keep the last GOOD reading across a later
   failure on purpose: the window labels it stale instead of going blank. */
char weather_text[24] = "";
unsigned int weather_last_tick = 0;
int weather_tried_once = 0;
int weather_temp_c = 0, weather_code10 = 0, weather_have = 0;
int weather_state = WX_NONE;
char weather_err[48] = "";
char geo_lat[16] = "", geo_lon[16] = "", geo_city[24] = "";
int geo_have = 0;
char loc_name[LOC_NAME_MAX] = "", loc_lat[16] = "", loc_lon[16] = "";
int loc_have = 0;
char loc_err[48] = "";
const char *weather_state_name(int st){
    return st == WX_OK ? "ok" : st == WX_OFFLINE ? "offline" : st == WX_TIMEOUT ? "timeout" : st == WX_FAILED ? "failed" : st == WX_BAD ? "bad" : "none";
}


/* A weather one-liner that has not started answering in ~15s will not.
   net.c's default reply budget (sized for local LLM generation, minutes)
   froze the whole desktop that long on a half-open connection. */
#define WX_REPLY_TIMEOUT_TICKS 1200

const char *weather_word(int code){
    if (code == 0) return "Clear";
    if (code <= 3) return "Cloudy";
    if (code <= 48) return "Fog";
    if (code <= 67) return "Rain";
    if (code <= 77) return "Snow";
    if (code <= 82) return "Showers";
    return "Storm";
}

/* v60: wind_weather_pct's source. Same thresholds as weather_word() above
   on purpose, so the sway always matches the word actually shown in the
   menu bar/dropdown, not a second guess at the same code. Percentages are
   a judgment call (no real wind-speed field exists to derive them from,
   see wind_weather_pct's comment), ordered calmest to strongest by what
   each condition plausibly implies about the air: Fog is stillest, Clear
   next, Cloudy is the unchanged pre-v60 baseline, then Snow/Rain/Showers/
   Storm climb from there. */
int wind_pct_for_weather_code(int code){
    if (code == 0) return 60;    /* Clear: calm */
    if (code <= 3) return 100;   /* Cloudy: baseline, matches pre-v60 sway */
    if (code <= 48) return 40;   /* Fog: stillest air */
    if (code <= 67) return 130;  /* Rain */
    if (code <= 77) return 90;   /* Snow: typically calmer than rain */
    if (code <= 82) return 150;  /* Showers */
    return 200;                  /* Storm: strongest sway */
}

/* v65 (0.62.0): weather particle overlay, direct follow-up to v60's wind-
   amplitude scaling, the "beyond wind" half of the same real request.
   Real, visible rain streaks / snow dots for the condition codes that
   warrant one, no new subsystem: a small fixed-size particle array
   (WEATHER_PARTICLE_COUNT, same shape as wind_base's own fixed cost
   budget), advanced and drawn once per wind tick (weather_fx_tick, called
   from the same ~20fps loop in gui_run that already drives wind_phase),
   same cost class as the existing wind-sway sampler it rides alongside.
   weather_fx_kind reuses weather_word()'s own threshold boundaries
   exactly (same code10/10 input, same cutoffs), on purpose, so the
   overlay always matches the condition word the menu bar/dropdown already
   shows, never a second guess at the same fetched field: Clear/Cloudy/Fog
   get no overlay, Rain/Showers/Storm get rain streaks, Snow gets snow
   dots. Honest scope note: particles are only ever drawn inside
   WIND_TOP_ROW..WIND_HORIZON_ROW, the same swaying sky band the wind tick
   already repaints in full every frame, which is what erases last frame's
   particles with no separate restore/dirty-rect logic needed (the same
   trick the wind redraw itself already relies on). Extending particles
   across the dock/ground band below the horizon would need the same kind
   of repaint plumbing gui_redraw_dock_band's own band cache has, real
   follow-up, not attempted here. A NIC-less boot (v86) never calls
   weather_fetch, so weather_have stays 0 and weather_fx_tick is a no-op,
   same "nothing fabricated" contract v56/v60 already established for
   every other weather-derived effect in this file. */

struct weather_particle weather_particles[WEATHER_PARTICLE_COUNT];
int weather_particles_seeded = 0;
static unsigned int weather_rng = 0;

/* Same tiny LCG keyrate's own word generator already uses (no rand()/no
   libc in this freestanding build); good enough for particle scatter, not
   for anything security-sensitive, same honesty note that code carries. */
static unsigned int weather_rand(void){
    weather_rng = weather_rng * 1103515245u + 12345u;
    return (weather_rng >> 16) & 0x7fff;
}

/* Pure: same answer every time for the same code, no globals touched, so
   weatherfxtest can call this directly. Same cutoffs as weather_word()
   above, stated once rather than re-derived: <=3 Clear/Cloudy, <=48 Fog,
   <=67 Rain, <=77 Snow, <=82 Showers, else Storm. */
int weather_fx_kind(int code){
    if (code <= 3) return WEATHER_FX_NONE;    /* Clear, Cloudy */
    if (code <= 48) return WEATHER_FX_NONE;   /* Fog */
    if (code <= 67) return WEATHER_FX_RAIN;   /* Rain */
    if (code <= 77) return WEATHER_FX_SNOW;   /* Snow */
    return WEATHER_FX_RAIN;                   /* Showers, Storm */
}

void weather_particles_seed(int w){
    weather_rng = ticks() ? ticks() : 1;
    for (int i = 0; i < WEATHER_PARTICLE_COUNT; i++){
        weather_particles[i].x = (int)(weather_rand() % (unsigned int)(w > 0 ? w : 1));
        weather_particles[i].y = WIND_TOP_ROW + (int)(weather_rand() % (unsigned int)(WIND_HORIZON_ROW - WIND_TOP_ROW));
        weather_particles[i].speed = 4 + (int)(weather_rand() % 5);
    }
    weather_particles_seeded = 1;
}

/* Advances every particle one tick and draws it directly (physical
   coords via window_pixel_phys, same as the wallpaper's own draw path,
   so it scales correctly at any window_scale()). Colors stay inside the
   Mojave palette on purpose: a light silvery-sand rain streak and an off-
   white (the dock tray's own cream) snow dot, never a saturated or cold-
   blue tone. `kind`/`w` passed explicitly rather than read from globals
   so weatherfxtest can drive this deterministically. */
/* v71 (0.65.0): every particle pixel goes through this clip, and the
   wrap-around check now runs BEFORE the draw, not after. The real bug
   this fixes (Joshua's "dashed glitch bar at the horizon", reproduced
   headlessly: 314 streak-coloured pixels on physical rows 790..804 after
   ten idle seconds under a Rain reading, 12 above): a rain particle at
   logical row 394 with speed 8 was advanced to 402, drawn there, and only
   THEN wrapped back to the top. Rows 395..402 are below WIND_HORIZON_ROW,
   outside the band the wind tick repaints every frame, so nothing ever
   erased those stamps; each streak that crossed the horizon left a
   permanent 4-pixel diagonal at a random x, accumulating into exactly the
   dashed full-width bar seen live. Snow had the same leak one row deep
   (y advanced to 395, dotted at physical 790). Clipping to the band is
   the real contract the v65 entry already stated ("particles only ever
   draw inside WIND_TOP_ROW..WIND_HORIZON_ROW"), now enforced per pixel
   rather than assumed from the reset logic. */
static inline void weather_fx_plot(int px, int py, int w, int sc, unsigned int color){
    if (py < WIND_TOP_ROW * sc || py >= WIND_HORIZON_ROW * sc) return;
    if (px < 0 || px >= w * sc) return;
    window_pixel_phys(px, py, color);
}

void weather_fx_tick_kind(int kind, int w){
    if (kind == WEATHER_FX_NONE) return;
    if (!weather_particles_seeded) weather_particles_seed(w);
    int sc = (int)window_scale();
    for (int i = 0; i < WEATHER_PARTICLE_COUNT; i++){
        struct weather_particle *p = &weather_particles[i];
        if (kind == WEATHER_FX_RAIN){
            p->y += p->speed;                       /* fast, mostly-vertical fall */
            p->x += 1;                               /* a slight real wind-blown slant */
        } else {
            p->y += 1;                               /* snow drifts, much slower than rain */
            if (weather_rand() & 1) p->x += ((i & 1) * 2 - 1); /* gentle side-to-side drift */
        }
        if (p->y >= WIND_HORIZON_ROW || p->x < 0 || p->x >= w){
            p->x = (int)(weather_rand() % (unsigned int)(w > 0 ? w : 1));
            p->y = WIND_TOP_ROW;
            p->speed = 4 + (int)(weather_rand() % 5);
        }
        int x0 = p->x * sc, y0 = p->y * sc;
        if (kind == WEATHER_FX_RAIN){
            for (int s = 0; s < 4; s++)
                weather_fx_plot(x0 - s, y0 - s * sc, w, sc, 0x00C9C0B4); /* light silvery-sand streak, in-palette */
        } else {
            weather_fx_plot(x0, y0, w, sc, 0x00EFEBE4);           /* off-white dot, the dock tray's own cream */
            if (sc > 1) weather_fx_plot(x0 + 1, y0, w, sc, 0x00EFEBE4);
        }
    }
}

/* The real, live entry point: derives kind from the actually-fetched
   weather_code10, "nothing fabricated" the same way weather_fetch's own
   callers already require. */
void weather_fx_tick(int w){
    weather_fx_tick_kind(weather_have ? weather_fx_kind(weather_code10 / 10) : WEATHER_FX_NONE, w);
}

/* Pulls a number for `key` from inside the "current":{...} object. The
   units object earlier in the same reply has the same keys with string
   values ("°C"), so the search has to start after "current":{ or it would
   read the wrong one. */
int json_current_number(const char *json, const char *key, int *out_x10){
    const char *p = json;
    const char *cur = 0;
    while (*p) { if (p[0]=='"' && p[1]=='c' && p[2]=='u' && p[3]=='r' && p[4]=='r' && p[5]=='e' && p[6]=='n' && p[7]=='t' && p[8]=='"' && p[9]==':' && p[10]=='{') { cur = p + 11; break; } p++; }
    if (!cur) return 0;
    for (p = cur; *p && *p != '}'; p++) {
        const char *k = key; const char *q = p;
        if (*q != '"') continue;
        q++;
        while (*k && *q == *k) { q++; k++; }
        if (*k || *q != '"' || q[1] != ':') continue;
        q += 2;
        int neg = 0; if (*q == '-') { neg = 1; q++; }
        int whole = 0, frac = 0, seen_dot = 0;
        while ((*q >= '0' && *q <= '9') || *q == '.') {
            if (*q == '.') { seen_dot = 1; q++; continue; }
            if (!seen_dot) whole = whole * 10 + (*q - '0');
            else if (frac == 0) { frac = (*q - '0'); }
            q++;
        }
        int v = whole * 10 + frac;
        *out_x10 = neg ? -v : v;
        return 1;
    }
    return 0;
}

/* v71 (0.65.0): real location, not a constant. weather_fetch used to hard-
   code latitude=49.28&longitude=-123.12 (downtown Vancouver) in its Open-
   Meteo URL, so every weather-derived effect in this file (menu bar text,
   v56 dropdown, v60 wind amplitude, v65 particles and tint) reported a
   place ~40km from where this machine actually sits (Langley, per both
   Joshua's own phone and a real `curl http://ip-api.com/json/` from the
   host, ROADMAP.md's "Real find" entry). Open-Meteo itself was never
   wrong, it answered honestly for the coordinates it was given; the
   coordinates were the bug. ip-api.com answers on plain HTTP (no TLS in
   this kernel), so this is the same http_get shape the weather call
   already uses. Looked up once per boot and cached (a public IP doesn't
   move mid-session; a failed lookup is retried on the next ten-minute
   weather cycle). The lat/lon are kept as the exact numeric TEXT ip-api
   returned (json_extract_number_text) and spliced straight into the URL,
   no float parse/format round trip to lose digits in. No fallback
   constant on purpose: with no real location there is no weather fetch,
   the same "nothing fabricated" contract v56/v60/v65 already hold to for
   a NIC-less boot. Both values are mirrored to serial (`geo=`/`wxurl=`)
   so tools/geo-check.sh can prove headlessly, against the host's own
   ip-api answer, that the URL really carries the dynamic location.
   (geo_* and the loc_* Settings-location override are defined at the top
   of this file; settings_load in kernel.c fills loc_* through weather.h.) */
/* Open-Meteo's own geocoding endpoint (the same house the weather forecast
   already comes from), through the exact http_get_timeout/wx_override_host
   plumbing geo_fetch and weather_fetch_inner already use -- one more host
   name, same request shape, nothing new. `query` is bounded the same way
   every other Settings text field is (settings_prompt_line's max param);
   spaces are percent-encoded since a city name is likely to have one.
   No result, a bad reply, or no network all fail cleanly with loc_err set
   and loc_have/geo_have untouched -- never a fabricated coordinate, never
   a panic. */
static void loc_err_set(const char *m){   /* bounded: loc_err is 48 bytes */
    unsigned i = 0; while (m[i] && i < sizeof loc_err - 1) { loc_err[i] = m[i]; i++; } loc_err[i] = 0;
}
int loc_geocode(const char *query){
    loc_err[0] = 0;
    if (!query[0]) { loc_err_set("empty"); return 0; }
    if (!net_init(0x0A00020F)) { loc_err_set("no network card"); return 0; }
    static char path[112];
    int p = 0; const char *s;
    for (s = "/v1/search?name="; *s; s++) path[p++] = *s;
    for (const char *c = query; *c && p < 96; c++) {
        unsigned char ch = (unsigned char)*c; /* percent-encode all but [A-Za-z0-9] so '&', '#', '%' can't reshape the query */
        if ((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) path[p++] = (char)ch;
        else { path[p++]='%'; path[p++]="0123456789ABCDEF"[ch >> 4]; path[p++]="0123456789ABCDEF"[ch & 15]; }
    }
    for (s = "&count=1"; *s; s++) path[p++] = *s;
    path[p] = 0;
    static char body[1024];
    int n = wx_override_host[0] ? http_get_timeout(wx_override_host, path, wx_override_port, body, sizeof(body) - 1, WX_REPLY_TIMEOUT_TICKS)
                                 : http_get_timeout("geocoding-api.open-meteo.com", path, 80, body, sizeof(body) - 1, WX_REPLY_TIMEOUT_TICKS);
    if (n <= 0 || http_last_status() != 200) {
        int e = net_last_error();
        const char *m = (n < 0 || e == NET_ERR_REPLY_TIMEOUT || e == NET_ERR_ARP_TIMEOUT || e == NET_ERR_DNS_TIMEOUT || e == NET_ERR_CONNECT_TIMEOUT) ? "network unreachable" : "geocoding request failed";
        loc_err_set(m);
        return 0;
    }
    body[n] = 0;
    char lat[16], lon[16], name[LOC_NAME_MAX];
    if (!json_extract_number_text(body, "latitude", lat, sizeof(lat)) ||
        !json_extract_number_text(body, "longitude", lon, sizeof(lon))) {
        loc_err_set("location not found");
        return 0;
    }
    if (!json_extract_string(body, "name", name, sizeof(name))) name[0] = 0;
    int i;
    for (i = 0; lat[i]; i++) loc_lat[i] = lat[i]; loc_lat[i] = 0;
    for (i = 0; lon[i]; i++) loc_lon[i] = lon[i]; loc_lon[i] = 0;
    for (i = 0; name[i] && i < LOC_NAME_MAX - 1; i++) loc_name[i] = name[i]; loc_name[i] = 0;
    loc_have = 1;
    /* Same fields weather_fetch_inner/wall_fetch already read -- setting
       these here means neither one needs to know a manual override even
       exists. */
    for (i = 0; loc_lat[i]; i++) geo_lat[i] = loc_lat[i]; geo_lat[i] = 0;
    for (i = 0; loc_lon[i]; i++) geo_lon[i] = loc_lon[i]; geo_lon[i] = 0;
    for (i = 0; loc_name[i] && i < 23; i++) geo_city[i] = loc_name[i]; geo_city[i] = 0;
    geo_have = 1;
    serial_puts("locgeo="); serial_puts(loc_lat); serial_puts(","); serial_puts(loc_lon); serial_puts(" name="); serial_puts(loc_name); serial_puts("\n");
    return 1;
}
/* Turns the net/http layer's last failure into a window state plus a short
   human detail. `what` names the request ("location" / "forecast"). */
static void weather_set_error(int st, const char *what, const char *detail){
    weather_state = st;
    int p = 0;
    for (const char *c = what; *c && p < 46; c++) weather_err[p++] = *c;
    if (*detail && p < 45) { weather_err[p++] = ':'; weather_err[p++] = ' '; }
    for (const char *c = detail; *c && p < 47; c++) weather_err[p++] = *c;
    weather_err[p] = 0;
}
static void weather_classify_http_failure(const char *what, int n){
    int e = net_last_error();
    if (n < 0 || e == NET_ERR_REPLY_TIMEOUT) {
        if (e == NET_ERR_ARP_TIMEOUT) weather_set_error(WX_OFFLINE, what, "no route to the network");
        else if (e == NET_ERR_DNS_TIMEOUT || e == NET_ERR_CONNECT_TIMEOUT || e == NET_ERR_REPLY_TIMEOUT) weather_set_error(WX_TIMEOUT, what, net_error_name(e));
        else weather_set_error(WX_FAILED, what, net_error_name(e));
        return;
    }
    int st = http_last_status();
    if (st && st != 200) {
        char d[12] = "HTTP "; int q = 5;
        d[q++] = '0' + (st / 100) % 10; d[q++] = '0' + (st / 10) % 10; d[q++] = '0' + st % 10; d[q] = 0;
        weather_set_error(WX_BAD, what, d);
    } else weather_set_error(WX_BAD, what, n == 0 ? "empty reply" : "unreadable reply");
}

int geo_fetch(void){
    static char body[1024];
    int n = wx_override_host[0] ? http_get_timeout(wx_override_host, "/json/", wx_override_port, body, sizeof(body) - 1, WX_REPLY_TIMEOUT_TICKS)
                                : http_get_timeout("ip-api.com", "/json/", 80, body, sizeof(body) - 1, WX_REPLY_TIMEOUT_TICKS);
    if (n <= 0 || http_last_status() != 200) { weather_classify_http_failure("location", n); return 0; }
    body[n] = 0;
    char lat[16], lon[16];
    if (!json_extract_number_text(body, "lat", lat, sizeof(lat)) ||
        !json_extract_number_text(body, "lon", lon, sizeof(lon))) { weather_set_error(WX_BAD, "location", "unreadable reply"); return 0; }
    int i;
    for (i = 0; lat[i]; i++) geo_lat[i] = lat[i]; geo_lat[i] = 0;
    for (i = 0; lon[i]; i++) geo_lon[i] = lon[i]; geo_lon[i] = 0;
    if (!json_extract_string(body, "city", geo_city, sizeof(geo_city))) geo_city[0] = 0;
    geo_have = 1;
    serial_puts("geo="); serial_puts(geo_lat); serial_puts(","); serial_puts(geo_lon); serial_puts("\n");
    return 1;
}

#include "weather_extra.h"
static int weather_fetch_inner(void){
    weather_err[0] = 0;
    if (!net_init(0x0A00020F)) { weather_set_error(WX_OFFLINE, "no network card", ""); return 0; }
    if (!geo_have && !geo_fetch()) return 0; /* v71: no real location, no fetch, nothing fabricated */
    static char body[6144]; /* the 24-hour and 7-day reply is ~2.7KB; was 2048 */
    static char path[640]; /* was 320: the field list below is ~330 bytes plus the position; http.c's request buffer is 1024 */
    { int p = 0; const char *s;
      for (s = "/v1/forecast?latitude="; *s; s++) path[p++] = *s;
      for (s = geo_lat; *s; s++) path[p++] = *s;
      for (s = "&longitude="; *s; s++) path[p++] = *s;
      for (s = geo_lon; *s; s++) path[p++] = *s;
      /* One request for everything the Weather window shows: now, the next 24 hours and seven
         days with sunrise, sunset, UV and chance of rain. The real reply is ~2.7KB, inside the
         6144-byte body buffer above; forecast_hours=24 starts at the current hour. */
      for (s = "&current=temperature_2m,apparent_temperature,relative_humidity_2m,wind_speed_10m,weather_code,pressure_msl,visibility,is_day"
               "&hourly=temperature_2m,precipitation_probability,weather_code,is_day"
               "&daily=weather_code,temperature_2m_max,temperature_2m_min,sunrise,sunset,uv_index_max,precipitation_probability_max"
               "&forecast_days=7&forecast_hours=24&timezone=auto"; *s && p < 638; s++) path[p++] = *s;
      path[p] = 0; }
    serial_puts("wxurl="); serial_puts(path); serial_puts("\n");
    int n = wx_override_host[0] ? http_get_timeout(wx_override_host, path, wx_override_port, body, sizeof(body) - 1, WX_REPLY_TIMEOUT_TICKS)
                                : http_get_timeout("api.open-meteo.com", path, 80, body, sizeof(body) - 1, WX_REPLY_TIMEOUT_TICKS);
    if (n <= 0 || http_last_status() != 200) { weather_classify_http_failure("forecast", n); return 0; }
    body[n] = 0;
    int t10 = 0, code10 = 0;
    if (!json_current_number(body, "temperature_2m", &t10)) { weather_set_error(WX_BAD, "forecast", "unreadable reply"); return 0; }
    json_current_number(body, "weather_code", &code10);
    int t = (t10 >= 0 ? t10 + 5 : t10 - 5) / 10; /* round to whole degrees */
    weather_temp_c = t; weather_code10 = code10; weather_have = 1;
    wind_weather_pct = wind_pct_for_weather_code(code10 / 10); /* v60: real wind sway now follows real weather */
    wx_parse_extras(body);
    int p = 0;
    if (t < 0) { weather_text[p++] = '-'; t = -t; }
    if (t >= 10) weather_text[p++] = '0' + t / 10;
    weather_text[p++] = '0' + t % 10;
    weather_text[p++] = (char)0xF8; /* CP437 degree sign, present in both the hardware font and the fallback */
    weather_text[p++] = ' ';
    for (const char *w = weather_word(code10 / 10); *w; w++) weather_text[p++] = *w;
    weather_text[p] = 0;
    weather_state = WX_OK;
    /* What the window's secondary row and forecast row will be built from. */
    wx_serial_summary();
    serial_puts("wx="); serial_puts(weather_text); serial_puts("\n"); /* v71: tools/geo-check.sh asserts the fetch really landed, not just that the URL was built */
    return 1;
}
static void weather_write_file(void);
void weather_fetch(void){
    weather_last_tick = ticks();
    serial_puts("wxfetch\n"); /* tools/checks/weather-app-check.sh counts these: a failed fetch must not re-run on every repaint */
    weather_fetch_inner();
    /* One line per attempt, the state the window will show and why. */
    serial_puts("wxstate="); serial_puts(weather_state_name(weather_state));
    if (weather_err[0]) { serial_puts(" "); serial_puts(weather_err); }
    serial_puts("\n");
    weather_write_file();
}

/* 1.9.22: Weather is a ring-3 program (user/weather.c). It cannot see these
   statics, so every fetch, good or not, leaves WEATHER.TXT for it: one
   "key value" line per field, the seven forecast days as "d weekday code hi lo rain uv10 sunrise sunset" and the next 24 hours as hs/ht/hc/hp/hd lines. */
static void weather_write_file(void){
    char b[1536], *o = b; const char *c;
    #define WXPUT(str) do { for (c = (str); *c; c++) *o++ = *c; } while (0)
    #define WXNUM(key, v) do { WXPUT(key " "); o = wx_put_int(o, (v)); *o++ = '\n'; } while (0)
    WXPUT("state "); WXPUT(weather_state_name(weather_state)); *o++ = '\n';
    WXPUT("err "); WXPUT(weather_err); *o++ = '\n';
    WXPUT("city "); WXPUT(geo_city); *o++ = '\n';
    WXPUT("word "); WXPUT(weather_word(weather_code10 / 10)); *o++ = '\n';
    WXNUM("have", weather_have); WXNUM("temp", weather_temp_c); WXNUM("code", weather_code10 / 10);
    WXNUM("extra", wx_extra_have); WXNUM("feels", wx_feels_c); WXNUM("hum", wx_humidity); WXNUM("wind", wx_wind_kmh);
    o = wx_write_extras(o);
    vfs_replace_file("WEATHER.TXT", b, (unsigned int)(o - b));
    jt_data_stamp++;
}

/* The dock's Weather, blocking fallback only (a full window table). The window path in
   user/weather.c asks for refetches with SYS_REFRESH instead; here the app just runs once. */
void weather_ring3_open(void){
    if (!weather_tried_once) { weather_tried_once = 1; weather_fetch(); gui_menubar_force_redraw(); }
    unsigned int t0 = ticks();
    weather_ring3_run();
    weather_last_tick += ticks() - t0;
}

/* The shell's weather parse self-test (kernel.c run()): the canned reply's "current_units" block carries the same keys
   as strings before "current" carries the numbers, a naive key search reads the wrong one. 1 when -3.5 C / code 61. */
int weather_parse_check(int *t10_out, int *code10_out){
    static const char canned[] = "{\"latitude\":49.27,\"current_units\":{\"time\":\"iso8601\",\"temperature_2m\":\"\xb0" "C\",\"weather_code\":\"wmo code\"},"
                                 "\"current\":{\"time\":\"2026-09-14T07:40\",\"interval\":900,\"temperature_2m\":-3.5,\"weather_code\":61}}";
    int t10 = 999, code10 = 999;
    int ok_t = json_current_number(canned, "temperature_2m", &t10);
    int ok_c = json_current_number(canned, "weather_code", &code10);
    int ok = ok_t && ok_c && t10 == -35 && code10 == 610;
    *t10_out = t10; *code10_out = code10;
    return ok;
}

/* The shell's weatherfx self-test: the classification, then particle 0 moves under Rain and holds under Clear. The live
   overlay's particles are saved and put back around it. */
void weather_fx_check(int *ok_kind_out, int *moved_out, int *still_out){
    int save_seeded = weather_particles_seeded;
    struct weather_particle save_particles[WEATHER_PARTICLE_COUNT];
    for (int i = 0; i < WEATHER_PARTICLE_COUNT; i++) save_particles[i] = weather_particles[i];

    int ok_kind = weather_fx_kind(0) == WEATHER_FX_NONE   /* Clear */
               && weather_fx_kind(45) == WEATHER_FX_NONE  /* Fog */
               && weather_fx_kind(61) == WEATHER_FX_RAIN  /* Rain */
               && weather_fx_kind(73) == WEATHER_FX_SNOW  /* Snow */
               && weather_fx_kind(95) == WEATHER_FX_RAIN; /* Storm */

    weather_particles_seed(800);
    int y0 = weather_particles[0].y, x0 = weather_particles[0].x;
    for (int i = 0; i < 5; i++) weather_fx_tick_kind(WEATHER_FX_RAIN, 800);
    int moved_on_rain = (weather_particles[0].y != y0) || (weather_particles[0].x != x0);

    weather_particles[0].y = y0; weather_particles[0].x = x0;
    int y1 = weather_particles[0].y, x1 = weather_particles[0].x;
    for (int i = 0; i < 5; i++) weather_fx_tick_kind(WEATHER_FX_NONE, 800);
    int still_on_clear = (weather_particles[0].y == y1) && (weather_particles[0].x == x1);

    for (int i = 0; i < WEATHER_PARTICLE_COUNT; i++) weather_particles[i] = save_particles[i];
    weather_particles_seeded = save_seeded;
    *ok_kind_out = ok_kind; *moved_out = moved_on_rain; *still_out = still_on_clear;
}
