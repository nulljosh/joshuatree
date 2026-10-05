#ifndef WEATHER_EXTRA_H
#define WEATHER_EXTRA_H
/* Weather window data, split out of kernel.c in 2.10.0 (its line ceiling only goes down). Included
   by kernel.c just before weather_fetch_inner, so json_current_number, wx_round10's callers and
   serial_puts are already in scope. wx_parse_extras reads everything the Weather app shows beyond
   the menu bar reading; wx_write_extras appends it to WEATHER.TXT; wx_serial_summary prints the
   "wxextra=yes days=7 hours=24" line tools/checks/weather-app-check.sh reads. */
char *wx_put_int(char *o, int v);

/* Weather window extras, all from the same single Open-Meteo reply the
   menu bar reading comes from. wx_extra_have / wx_day_count stay 0 when a
   reply lacks them, and the window then leaves those cells out rather than
   inventing a value. A later failed fetch returns before touching any of
   this, so the stale face shows the whole last good reading. */
#define WX_DAYS 7
#define WX_HOURS 24
static int wx_extra_have = 0, wx_feels_c = 0, wx_humidity = 0, wx_wind_kmh = 0;
static int wx_day_count = 0;
static int wx_day_code[WX_DAYS], wx_day_hi[WX_DAYS], wx_day_lo[WX_DAYS], wx_day_wd[WX_DAYS];
/* 2.10.0: the richer Weather window. Per day: chance of rain (percent), peak UV (x10) and the
   sunrise and sunset as minutes after midnight; -1 where the reply had no such value. The next
   24 hours: start hour, temperature, WMO code, chance of rain (-1 unknown) and is_day. Current
   pressure (hPa, 0 unknown), visibility in tenths of a km (-1 unknown) and is_day. */
static int wx_day_pop[WX_DAYS], wx_day_uv[WX_DAYS], wx_day_rise[WX_DAYS], wx_day_set[WX_DAYS];
static int wx_hour_n = 0, wx_hour_start = 0, wx_hour_t[WX_HOURS], wx_hour_c[WX_HOURS], wx_hour_p[WX_HOURS], wx_hour_day[WX_HOURS];
static int wx_press = 0, wx_vis10 = -1, wx_isday = 1;
static int wx_round10(int v){ return (v >= 0 ? v + 5 : v - 5) / 10; }
/* Points just past the '[' of "key":[ inside the real "daily":{ (or "hourly":{) object.
   Same trap as json_current_number: "daily_units" repeats every key first,
   with string values, so the search has to start inside "daily":{ itself. */
static const char *json_section_array(const char *json, const char *tag, const char *key){
    const char *p = json, *d = 0;
    for (; *p; p++) { int i = 0; while (tag[i] && p[i] == tag[i]) i++; if (!tag[i]) { d = p + i; break; } }
    if (!d) return 0;
    for (p = d; *p && *p != '}'; p++) {
        if (*p != '"') continue;
        const char *q = p + 1, *k = key;
        while (*k && *q == *k) { q++; k++; }
        if (!*k && q[0] == '"' && q[1] == ':' && q[2] == '[') return q + 3;
    }
    return 0;
}
static const char *json_daily_array(const char *json, const char *key){ return json_section_array(json, "\"daily\":{", key); }
static const char *json_hourly_array(const char *json, const char *key){ return json_section_array(json, "\"hourly\":{", key); }
/* Minutes after midnight for each "YYYY-MM-DDTHH:MM" in an array (sunrise, sunset, the hourly
   time list); stops at the first entry that is not that shape, keeping what was real. */
static int json_array_clock(const char *p, int *out, int max){
    int n = 0;
    while (p && *p && *p != ']' && n < max) {
        while (*p == ' ' || *p == ',') p++;
        if (*p != '"') break;
        p++;
        int ok = 1;
        for (int i = 0; i < 16; i++) {
            if (i == 4 || i == 7) ok &= p[i] == '-';
            else if (i == 10) ok &= p[i] == 'T';
            else if (i == 13) ok &= p[i] == ':';
            else ok &= p[i] >= '0' && p[i] <= '9';
        }
        if (!ok) break;
        out[n++] = ((p[11]-'0')*10 + (p[12]-'0')) * 60 + (p[14]-'0')*10 + (p[15]-'0');
        p += 16; while (*p && *p != '"') p++; if (*p == '"') p++;
    }
    return n;
}
/* Up to `max` numbers from a JSON array body, each times ten with one
   decimal kept, the same fixed-point json_current_number uses. */
static int json_array_x10(const char *p, int *out, int max){
    int n = 0;
    while (p && *p && *p != ']' && n < max) {
        while (*p == ' ' || *p == ',') p++;
        int neg = 0, whole = 0, frac = 0, dot = 0, digits = 0;
        if (*p == '-') { neg = 1; p++; }
        while ((*p >= '0' && *p <= '9') || *p == '.') {
            if (*p == '.') dot = 1;
            else if (!dot) { whole = whole * 10 + (*p - '0'); digits = 1; }
            else if (dot == 1) { frac = *p - '0'; dot = 2; }
            p++;
        }
        if (!digits) break; /* null or a string: stop, keep what was real */
        out[n++] = neg ? -(whole * 10 + frac) : whole * 10 + frac;
    }
    return n;
}
/* Weekday (0 = Sunday) for each "YYYY-MM-DD" in the daily time array,
   Sakamoto's method. */
static int json_array_weekdays(const char *p, int *out, int max){
    static const int t[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    int n = 0;
    while (p && *p && *p != ']' && n < max) {
        while (*p == ' ' || *p == ',') p++;
        if (*p != '"') break;
        p++;
        int ok = 1; for (int i = 0; i < 10; i++) if (i == 4 || i == 7 ? p[i] != '-' : (p[i] < '0' || p[i] > '9')) ok = 0;
        if (!ok) break;
        int y = (p[0]-'0')*1000 + (p[1]-'0')*100 + (p[2]-'0')*10 + (p[3]-'0');
        int m = (p[5]-'0')*10 + (p[6]-'0'), d = (p[8]-'0')*10 + (p[9]-'0');
        if (m < 1 || m > 12) break;
        if (m < 3) y -= 1;
        out[n++] = (y + y/4 - y/100 + y/400 + t[m-1] + d) % 7;
        p += 10; if (*p == '"') p++;
    }
    return n;
}

/* Everything past the temperature and the condition: feels like, humidity, wind, pressure, visibility,
   the seven days and the next 24 hours. A reply that lacks a piece leaves that piece out (counts of
   0, -1 for an unknown value) and the window then says "Not reported" rather than inventing it. */
static void wx_parse_extras(const char *body){
    { int f10 = 0, h10 = 0, w10 = 0;
      wx_extra_have = json_current_number(body, "apparent_temperature", &f10)
                   && json_current_number(body, "relative_humidity_2m", &h10)
                   && json_current_number(body, "wind_speed_10m", &w10);
      wx_feels_c = wx_round10(f10); wx_humidity = wx_round10(h10); wx_wind_kmh = wx_round10(w10);
      int nw = json_array_weekdays(json_daily_array(body, "time"), wx_day_wd, WX_DAYS);
      int nc = json_array_x10(json_daily_array(body, "weather_code"), wx_day_code, WX_DAYS);
      int nh = json_array_x10(json_daily_array(body, "temperature_2m_max"), wx_day_hi, WX_DAYS);
      int nl = json_array_x10(json_daily_array(body, "temperature_2m_min"), wx_day_lo, WX_DAYS);
      int nd = nw; if (nc < nd) nd = nc; if (nh < nd) nd = nh; if (nl < nd) nd = nl;
      for (int i = 0; i < nd; i++) { wx_day_code[i] /= 10; wx_day_hi[i] = wx_round10(wx_day_hi[i]); wx_day_lo[i] = wx_round10(wx_day_lo[i]); }
      wx_day_count = nd;
      int tmp[WX_HOURS], k;
      for (k = 0; k < WX_DAYS; k++) wx_day_pop[k] = wx_day_uv[k] = wx_day_rise[k] = wx_day_set[k] = -1;
      int np = json_array_x10(json_daily_array(body, "precipitation_probability_max"), tmp, WX_DAYS);
      for (k = 0; k < np; k++) wx_day_pop[k] = tmp[k] / 10;
      np = json_array_x10(json_daily_array(body, "uv_index_max"), tmp, WX_DAYS);
      for (k = 0; k < np; k++) wx_day_uv[k] = tmp[k];
      json_array_clock(json_daily_array(body, "sunrise"), wx_day_rise, WX_DAYS);
      json_array_clock(json_daily_array(body, "sunset"), wx_day_set, WX_DAYS);
      { int st[1]; int has = json_array_clock(json_hourly_array(body, "time"), st, 1) == 1;
        int ht = json_array_x10(json_hourly_array(body, "temperature_2m"), wx_hour_t, WX_HOURS);
        int hc = json_array_x10(json_hourly_array(body, "weather_code"), wx_hour_c, WX_HOURS);
        int hn = ht < hc ? ht : hc;
        for (k = 0; k < WX_HOURS; k++) { wx_hour_p[k] = -1; wx_hour_day[k] = 1; }
        for (k = 0; k < hn; k++) { wx_hour_t[k] = wx_round10(wx_hour_t[k]); wx_hour_c[k] /= 10; }
        np = json_array_x10(json_hourly_array(body, "precipitation_probability"), tmp, WX_HOURS);
        for (k = 0; k < np; k++) wx_hour_p[k] = tmp[k] / 10;
        np = json_array_x10(json_hourly_array(body, "is_day"), tmp, WX_HOURS);
        for (k = 0; k < np; k++) wx_hour_day[k] = tmp[k] >= 5;
        wx_hour_start = has ? st[0] / 60 : 0; wx_hour_n = has ? hn : 0; }
      { int p10 = 0, v10 = 0, d10 = 10;
        wx_press = json_current_number(body, "pressure_msl", &p10) ? wx_round10(p10) : 0;
        wx_vis10 = json_current_number(body, "visibility", &v10) ? v10 / 1000 : -1; /* metres x10 -> tenths of a km */
        wx_isday = json_current_number(body, "is_day", &d10) ? d10 >= 5 : 1; } }
}
static void wx_serial_summary(void){
    serial_puts("wxextra="); serial_puts(wx_extra_have ? "yes" : "no");
    serial_puts(" days="); { char d[2] = { (char)('0' + wx_day_count), 0 }; serial_puts(d); }
    serial_puts(" hours="); { char d[3] = { (char)('0' + wx_hour_n / 10), (char)('0' + wx_hour_n % 10), 0 }; serial_puts(d); } serial_puts("\n");
}
/* The lines after "wind" in WEATHER.TXT: press, vis, isday, one "d" line per day, then the hour lines. */
static char *wx_write_extras(char *o){
    const char *c;
    #define WXPUT(str) do { for (c = (str); *c; c++) *o++ = *c; } while (0)
    #define WXNUM(key, v) do { WXPUT(key " "); o = wx_put_int(o, (v)); *o++ = '\n'; } while (0)
    WXNUM("press", wx_press); WXNUM("vis", wx_vis10); WXNUM("isday", wx_isday);
    for (int i = 0; i < wx_day_count; i++) {
        WXPUT("d "); o = wx_put_int(o, wx_day_wd[i]); *o++ = ' '; o = wx_put_int(o, wx_day_code[i]); *o++ = ' ';
        o = wx_put_int(o, wx_day_hi[i]); *o++ = ' '; o = wx_put_int(o, wx_day_lo[i]);
        *o++ = ' '; o = wx_put_int(o, wx_day_pop[i]); *o++ = ' '; o = wx_put_int(o, wx_day_uv[i]);
        *o++ = ' '; o = wx_put_int(o, wx_day_rise[i]); *o++ = ' '; o = wx_put_int(o, wx_day_set[i]); *o++ = '\n';
    }
    if (wx_hour_n > 0) {
        WXNUM("hs", wx_hour_start);
        #define WXARR(key, arr) do { WXPUT(key); for (int i = 0; i < wx_hour_n; i++) { *o++ = ' '; o = wx_put_int(o, arr[i]); } *o++ = '\n'; } while (0)
        WXARR("ht", wx_hour_t); WXARR("hc", wx_hour_c); WXARR("hp", wx_hour_p); WXARR("hd", wx_hour_day);
    }
    #undef WXPUT
    #undef WXNUM
    return o;
}
#endif
