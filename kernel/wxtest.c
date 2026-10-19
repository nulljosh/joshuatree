/* The weather and wallpaper self-tests (weathertest, weatherfxtest, weatherfxcliptest, windweathertest, weatherpaneltest,
 geotest, loctest, daynighttest, maptinttest, walltest): each drives weather.c or wallmap.c headless and prints a marker
 a check reads. Moved out of kernel.c's run() in 4.19.1; run() hands every line here after the network commands. */
#include "libc.h"
#include "console.h"
#include "serial.h"
#include "window.h"
#include "vfs.h"
#include "json.h"
#include "gui_prims.h"
#include "gui_paint.h"
#include "settings.h"
#include "weather.h"
#include "desktop.h"
#include "walltint.h"
#include "wallmap.h"
#include "selftest.h"

int wxtest_run(const char *line, char *arg) {
    (void)arg;
    if (0) {}
    else if (!strcmp(line, "weathertest")) {
        /* v43: the parser has one real trap, so it gets a real test: the
           reply's "current_units" object carries the same keys with STRING
           values ("°C") before the "current" object carries the numbers.
           A naive key search reads the wrong one. Negative and fractional
           temperatures are the other two edge cases a Vancouver winter
           will actually exercise. */
        int t10 = 999, code10 = 999;
        int ok = weather_parse_check(&t10, &code10);   /* weather.c: the canned reply through json_current_number */
        puts(ok ? "weather parse: -3.5C / code 61 through the units-block trap: ok\n" : "weather parse: FAILED\n");
        if (!ok) { puts("  t10="); putn((unsigned int)t10); puts(" code10="); putn((unsigned int)code10); puts("\n"); }
    }
    else if (!strcmp(line, "weatherfxtest")) {
        /* v65 (0.62.0): standing QA per CLAUDE.md's 4b, two real,
           discriminating checks. (1) weather_fx_kind is a pure function
           of a WMO code (see its own comment above, same cutoffs
           weather_word() uses), so Clear/Fog/Rain/Snow/Storm codes are
           checked directly against the classification a reverted feature
           would get wrong. (2) weather_fx_tick_kind actually has to move
           real particle state, not just classify a code correctly: seed
           a known particle set, tick it under a live Rain classification
           and confirm particle 0 really moved, then tick the same count
           under WEATHER_FX_NONE and confirm it does NOT move (the early-
           return path), proving the "Clear gets no overlay" contract is
           real, not just a comment. Global particle state is saved and
           restored around this so a real live overlay in progress isn't
           disturbed by running the test. */
        int ok_kind, moved_on_rain, still_on_clear;
        weather_fx_check(&ok_kind, &moved_on_rain, &still_on_clear);   /* weather.c: classification, then particles under Rain and Clear */
        int ok = ok_kind && moved_on_rain && still_on_clear;
        puts(ok ? "weatherfx: classification + rain moves/clear doesn't: ok\n" : "weatherfx: FAILED\n");
        if (!ok) {
            puts("  ok_kind="); putn((unsigned int)ok_kind);
            puts(" moved_on_rain="); putn((unsigned int)moved_on_rain);
            puts(" still_on_clear="); putn((unsigned int)still_on_clear); puts("\n");
        }

    }
    else if (!strcmp(line, "weatherfxcliptest")) {
        /* v71 (0.65.0): regression test for the horizon glitch bar, per
           CLAUDE.md's 4b. Real framebuffer, not just particle state: opens
           the desktop's own 960x540 scale-2 window, clears it to a flat
           colour, parks particles one logical row above WIND_HORIZON_ROW
           (rain at max speed, then snow), ticks each kind ONCE, and scans
           the physical rows from the horizon down. The pre-v71 tick drew a
           particle at its advanced position (up to 8 rows past the
           horizon) before wrapping it, so any streak/dot pixel found at or
           below WIND_HORIZON_ROW*sc is exactly the leak that painted the
           dashed bar on Joshua's live desktop. Also asserts the tick did
           draw real particle pixels somewhere inside the band, so a
           "fix" that simply stopped drawing would fail too. Particle
           globals saved/restored like weatherfxtest. */
        int save_seeded = weather_particles_seeded;
        struct weather_particle save_particles[WEATHER_PARTICLE_COUNT];
        for (int i = 0; i < WEATHER_PARTICLE_COUNT; i++) save_particles[i] = weather_particles[i];
        if (!window_open_scaled(960, 540, 32, 2)) { puts("no VGA device found or out of page tables\n"); }
        else {
            const unsigned int flat = 0x00202020;
            int sc = (int)window_scale(), w = (int)window_width();
            int leaked = 0, drawn = 0;
            for (int kind = WEATHER_FX_RAIN; kind <= WEATHER_FX_SNOW; kind++) {
                window_clear(flat);
                weather_particles_seeded = 1;
                for (int i = 0; i < WEATHER_PARTICLE_COUNT; i++) {
                    weather_particles[i].x = 40 + i * 24;
                    weather_particles[i].y = WIND_HORIZON_ROW - 1;
                    weather_particles[i].speed = 8;
                }
                weather_fx_tick_kind(kind, w);
                for (int py = WIND_HORIZON_ROW * sc; py < (WIND_HORIZON_ROW + 12) * sc; py++)
                    for (int px = 0; px < w * sc; px++)
                        if (window_get_pixel_phys(px, py) != flat) leaked++;
                for (int py = WIND_TOP_ROW * sc; py < WIND_HORIZON_ROW * sc; py++)
                    for (int px = 0; px < w * sc; px++)
                        if (window_get_pixel_phys(px, py) != flat) drawn++;
            }
            window_close();
            puts(drawn > 0 ? "weatherfx clip: particles really drawn inside the sky band: ok\n" : "weatherfx clip: nothing drawn at all (test not discriminating): FAILED\n");
            puts(leaked == 0 ? "weatherfx clip: zero particle pixels at or below the horizon: ok\n" : "weatherfx clip: FAILED, pixels leaked below WIND_HORIZON_ROW: ");
            if (leaked) { putn((unsigned int)leaked); puts("\n"); }
        }
        weather_particles_seeded = save_seeded;
        for (int i = 0; i < WEATHER_PARTICLE_COUNT; i++) weather_particles[i] = save_particles[i];
    }
    else if (!strcmp(line, "windweathertest")) {
        /* v60 gap fix: verification that pass was a boot-time direct-call
           dump of three gui_wind_shift() samples, reverted before commit,
           nothing left in the suite. Real, discriminating logic to pin
           down: wind_pct_for_weather_code's seven WMO-code buckets, the
           exact percentages ROADMAP.md's own table names (Fog stillest at
           40%, then Clear/Cloudy/Snow/Rain/Showers/Storm climbing to
           200%), every boundary value, not just one code per bucket. */
        int ok =
            wind_pct_for_weather_code(0)  == 60  && wind_pct_for_weather_code(3)  == 100 &&
            wind_pct_for_weather_code(4)  == 40  && wind_pct_for_weather_code(48) == 40  &&
            wind_pct_for_weather_code(49) == 130 && wind_pct_for_weather_code(67) == 130 &&
            wind_pct_for_weather_code(68) == 90  && wind_pct_for_weather_code(77) == 90  &&
            wind_pct_for_weather_code(78) == 150 && wind_pct_for_weather_code(82) == 150 &&
            wind_pct_for_weather_code(83) == 200 && wind_pct_for_weather_code(95) == 200;
        puts(ok ? "wind_pct_for_weather_code: every WMO bucket boundary maps to its real percent: ok\n"
                : "wind_pct_for_weather_code: FAILED\n");
    }
    else if (!strcmp(line, "weatherpaneltest")) {
        /* v56 gap fix: weathertest (above) proves weather_fetch's JSON
           parse; nothing proved the dropdown itself once shipped, that
           pass's verification was a boot-time pixel dump into fixed RAM,
           reverted before commit. Two real, discriminating checks: (1)
           weather_word()'s WMO-code bucket mapping, the same function the
           dropdown's condition word and v60's wind multiplier both depend
           on, every boundary named in ROADMAP.md's own table; (2)
           gui_draw_weather_panel() actually paints its own bg chrome and,
           only once weather_have is set, real antialiased glyph ink for
           the line it claims to show, not a blank rect.

           v72 gap fix, root-caused with a real DEBUG serial dump, not
           guessed: both checks below failed on pristine v70/v71 HEAD, and
           the panel itself was fine, painting real chrome both times
           (confirmed: the interior point now used, x0+40/y0+20 or
           x0+40/y0+40, read back the real fill color 0x002C2C2E in both
           the no-data and live-data cases). Two real, separate causes,
           both predating this test's own geometry:
           (1) the old (x0+4, y0+4) corner probe predates v66's
           GUI_FLYOUT_RADIUS=14 rounding of this panel. At radius 14 with
           a 3px AA band, the point 4px in from each edge is genuinely
           OUTSIDE the corner arc (distance from the arc centre (14,14) is
           sqrt(200)=~14.14, past the 14px radius), so
           gui_rounded_rect_on_wallpaper's own `continue` leaves it
           untouched -- it read back whatever was there before the panel
           drew (the test's own pre-clear color), not the panel's fault.
           (2) the (x0, y0) "border" probe checked for a border color that
           v66 deliberately removed from this exact panel: see
           GUI_FLYOUT_RADIUS's own comment, "these three flyouts... were
           the one place still... with a manually drawn 1px border...
           zero separate stroke line" is the whole point of that pass.
           There has been no border to find at (x0, y0) since v66; that
           corner pixel is also outside the radius per (1) regardless.
           Real fix is in this test, not the panel: probe a genuine
           interior point clear of both the corner radius and the
           top-edge AA band, and stop asserting a border this panel was
           intentionally redesigned not to have. */
        int ok_word =
            !strcmp(weather_word(0),  "Clear")   && !strcmp(weather_word(3),  "Cloudy") &&
            !strcmp(weather_word(4),  "Fog")     && !strcmp(weather_word(48), "Fog")    &&
            !strcmp(weather_word(49), "Rain")    && !strcmp(weather_word(67), "Rain")   &&
            !strcmp(weather_word(68), "Snow")    && !strcmp(weather_word(77), "Snow")   &&
            !strcmp(weather_word(78), "Showers") && !strcmp(weather_word(82), "Showers")&&
            !strcmp(weather_word(83), "Storm")   && !strcmp(weather_word(95), "Storm");
        puts(ok_word ? "weather_word: every WMO bucket boundary maps correctly: ok\n" : "weather_word: FAILED\n");

        if (!window_open(800, 600, 32)) { puts("no VGA device found or out of page tables\n"); }
        else {
            unsigned int bg = 0x002C2C2E;
            weather_hit_x0 = -1; /* force the fallback geometry branch, same formula gui_draw_weather_panel uses */
            int x0 = (int)window_width() - WEATHER_W - 200;
            if (x0 + WEATHER_W > (int)window_width() - 4) x0 = (int)window_width() - WEATHER_W - 4;
            int y0 = GUI_MENUBAR_H;

            weather_have = 0;
            window_clear(0x00111111);
            gui_draw_weather_panel();
            int chrome_empty = window_get_pixel(x0 + 40, y0 + 20) == bg;
            puts(chrome_empty ? "weather panel (no data): chrome painted: ok\n" : "weather panel (no data): FAILED\n");

            weather_have = 1; weather_temp_c = -3; weather_code10 = 610;
            int wi = 0; const char *seed = "-3\xf8 Rain";
            while (seed[wi] && wi < (int)sizeof(weather_text) - 1) { weather_text[wi] = seed[wi]; wi++; }
            weather_text[wi] = 0;
            window_clear(0x00111111);
            gui_draw_weather_panel();
            int chrome_ok = window_get_pixel(x0 + 40, y0 + 40) == bg;
            int ink = 0;
            for (int dx = 0; dx < WEATHER_W - 24 && !ink; dx++)
                if (window_get_pixel(x0 + 12 + dx, y0 + 10) != bg) ink = 1;
            puts((chrome_ok && ink) ? "weather panel (live data): chrome + real ink drawn: ok\n" : "weather panel (live data): FAILED\n");
            window_close();
            weather_have = 0; weather_text[0] = 0; /* leave state clean for anything run after */
        }
    }
    else if (!strcmp(line, "geotest")) {
        /* v71 (0.65.0): the parser half of the real-location fix, pure and
           offline (tools/geo-check.sh is the live-network half). A fixed
           ip-api-shaped body, including the traps a sloppy match would
           trip on: "latitude"/"longitude" keys earlier in the text (the
           Open-Meteo reply's own key names, which "lat"/"lon" must NOT
           match inside), a negative longitude, a string-valued key that
           must be rejected as not-a-number, and a missing key. Asserts the
           exact text ip-api returned comes back byte-for-byte, since that
           text is what weather_fetch splices into its URL. */
        const char *body = "{\"status\":\"success\",\"latitude\":\"trap\",\"longitude\":1,\"city\":\"Langley\",\"lat\":49.0983,\"lon\":-122.6498,\"zip\":\"V3A\"}";
        char lat[16], lon[16], city[24], bad[16], none[16];
        unsigned int nlat = json_extract_number_text(body, "lat", lat, sizeof(lat));
        unsigned int nlon = json_extract_number_text(body, "lon", lon, sizeof(lon));
        unsigned int nbad = json_extract_number_text(body, "zip", bad, sizeof(bad));
        unsigned int nnone = json_extract_number_text(body, "isp", none, sizeof(none));
        unsigned int ncity = json_extract_string(body, "city", city, sizeof(city));
        int ok_lat = nlat == 7 && !strcmp(lat, "49.0983");
        int ok_lon = nlon == 9 && !strcmp(lon, "-122.6498");
        int ok_reject = nbad == 0 && nnone == 0;
        int ok_city = ncity == 7 && !strcmp(city, "Langley");
        puts(ok_lat ? "geo lat: exact text extracted, not matched inside \"latitude\": ok\n" : "geo lat: FAILED\n");
        puts(ok_lon ? "geo lon: negative value extracted byte-exact: ok\n" : "geo lon: FAILED\n");
        puts(ok_reject ? "geo: string value and missing key both rejected: ok\n" : "geo reject: FAILED\n");
        puts(ok_city ? "geo city: ok\n" : "geo city: FAILED\n");
        if (!(ok_lat && ok_lon)) { puts("  lat="); puts(lat); puts(" lon="); puts(lon); puts("\n"); }
    }
    else if (!strcmp(line, "loctest")) {
        /* v0.85.5: Settings Location, headless and deterministic --
           tools/checks/location-check.py boots this against a local fake
           geocoding-api.open-meteo.com server (same wxhost= override
           weather-app-check.sh's mock already uses for ip-api/Open-Meteo),
           never real internet, so this never flakes on CI's own network.
           Three real things proven: one, a real geocode through
           loc_geocode lands in the loc and geo globals exactly the way a
           Settings save would; two, settings_save and settings_load
           round-trip it through SETTINGS.TXT, the same file wind/dock/wall
           already prove elsewhere, simulating a reboot the same way the
           shell's own mail round trip test above simulates one for
           MAIL.TXT; three, an unresolvable query fails cleanly (loc_err
           set, nothing overwritten, no crash) rather than fabricating a
           coordinate. */
        serial_puts("loctest start\n");
        char save_name[LOC_NAME_MAX]; int si=0; while (loc_name[si]) { save_name[si]=loc_name[si]; si++; } save_name[si]=0;
        int ok1 = loc_geocode("Langley");
        int ok1b = ok1 && loc_have && loc_lat[0] && loc_lon[0] && loc_name[0]
                   && !strcmp(geo_lat, loc_lat) && !strcmp(geo_lon, loc_lon) && !strcmp(geo_city, loc_name) && geo_have;
        serial_puts(ok1b ? "loc geocode: real lookup lands in loc_*/geo_*: ok\n" : "loc geocode: FAILED\n");

        char want_name[LOC_NAME_MAX], want_lat[16], want_lon[16];
        { int i=0; while (loc_name[i]) { want_name[i]=loc_name[i]; i++; } want_name[i]=0; }
        { int i=0; while (loc_lat[i]) { want_lat[i]=loc_lat[i]; i++; } want_lat[i]=0; }
        { int i=0; while (loc_lon[i]) { want_lon[i]=loc_lon[i]; i++; } want_lon[i]=0; }
        settings_save();
        loc_have = 0; loc_name[0] = 0; loc_lat[0] = 0; loc_lon[0] = 0;
        geo_have = 0; geo_lat[0] = 0; geo_lon[0] = 0; geo_city[0] = 0;
        settings_load();
        int ok2 = loc_have && !strcmp(loc_name, want_name) && !strcmp(loc_lat, want_lat) && !strcmp(loc_lon, want_lon)
                  && geo_have && !strcmp(geo_lat, want_lat) && !strcmp(geo_lon, want_lon) && !strcmp(geo_city, want_name);
        serial_puts(ok2 ? "loc persist: settings_save/settings_load round trip (simulated reboot): ok\n" : "loc persist: FAILED\n");

        loc_err[0] = 0;
        int ok3 = !loc_geocode("Nowhereville") && loc_err[0] && loc_have && !strcmp(loc_name, want_name);
        serial_puts(ok3 ? "loc not-found: fails clean, error set, nothing overwritten: ok\n" : "loc not-found: FAILED\n");

        loc_err[0] = 0;
        int ok4 = !loc_geocode("");
        serial_puts(ok4 ? "loc empty input: rejected, no crash: ok\n" : "loc empty input: FAILED\n");

        int p=0; while (save_name[p]) { loc_name[p]=save_name[p]; p++; } loc_name[p]=0; /* restore whatever was there before this test ran */
        serial_puts((ok1b && ok2 && ok3 && ok4) ? "loctest PASS\n" : "loctest FAIL\n");
    }
    else if (!strcmp(line, "daynighttest")) {
        /* v65 (0.62.0): standing QA per CLAUDE.md's 4b. daynight_calc and
           gui_daynight_tint_pct are both pure functions of an explicit
           hour/pct (see their own comments above, deliberately shaped
           this way for exactly this test), so this pins hour=2 (deep
           night) against hour=14 (real midday) directly, no faked
           hardware needed, the same boot-time direct-call trick this
           suite's other tests use, just via a pure function instead of a
           temporary kmain hook. Three real, discriminating checks a
           reverted feature would fail: the two hours actually produce a
           different color for the same input pixel, the night one is
           genuinely darker (not just different), and it stays inside
           this repo's own "never cold black-and-blue" rule (blue channel
           never exceeds red on a warm test color, at either hour). */
        unsigned int test_color = 0x00C97A3E; /* a plausible warm sunset tone from this file's own photo */
        int night2 = 0, day2 = 0, night14 = 0, day14 = 0;
        daynight_calc(2, &night2, &day2);
        daynight_calc(14, &night14, &day14);
        unsigned int c2 = gui_daynight_tint_pct(test_color, night2, day2);
        unsigned int c14 = gui_daynight_tint_pct(test_color, night14, day14);
        int r2 = (int)((c2 >> 16) & 0xFF), g2 = (int)((c2 >> 8) & 0xFF), b2 = (int)(c2 & 0xFF);
        int r14 = (int)((c14 >> 16) & 0xFF), g14 = (int)((c14 >> 8) & 0xFF), b14 = (int)(c14 & 0xFF);
        int sum2 = r2 + g2 + b2, sum14 = r14 + g14 + b14;
        int ok = (c2 != c14) && (sum2 < sum14) && (b2 <= r2) && (b14 <= r14) && (night2 > night14) && (day14 >= day2);
        puts(ok ? "daynight: hour=2 darker+warm than hour=14, both differ: ok\n" : "daynight: FAILED\n");
        if (!ok) {
            puts("  c2="); puthex(c2); puts(" c14="); puthex(c14);
            puts(" sum2="); putn((unsigned int)sum2); puts(" sum14="); putn((unsigned int)sum14); puts("\n");
        }
    }
    else if (!strcmp(line, "maptinttest")) {
        /* v79 (0.68.0): standing QA per CLAUDE.md's 4b, for the map-only
           warm color grade added above (gui_map_tint). Three real,
           discriminating checks a reverted grade would fail: (1) a
           neutral OpenTopoMap-style gray actually shifts warm (red ends
           up strictly greater than blue, it started equal), (2) two
           distinct source colors (a road gray and a water blue) stay
           distinct after the grade -- proves this is a multiply, not a
           lerp-to-one-target that would collapse them together, (3) a
           near-white street-label background and a near-black label
           glyph both stay on their own end of the range (white stays
           above 200, black stays under 40) so the grade doesn't crush
           the contrast a real label needs to read. */
        unsigned int gray  = 0x00A0A0A0; /* plausible OpenTopoMap road/contour gray */
        unsigned int water = 0x006E9BC7; /* plausible OpenTopoMap water blue */
        unsigned int label_bg = 0x00F2F0E8;  /* near-white street-name background */
        unsigned int label_fg = 0x00202020;  /* near-black street-name glyph */
        unsigned int tg = gui_map_tint(gray), tw = gui_map_tint(water);
        unsigned int tbg = gui_map_tint(label_bg), tfg = gui_map_tint(label_fg);
        int gr = (int)((tg >> 16) & 0xFF), gb = (int)(tg & 0xFF);
        int bg_r = (int)((tbg >> 16) & 0xFF);
        int fg_r = (int)((tfg >> 16) & 0xFF);
        int ok = (gr > gb) && (tg != tw) && (tg != gray) && (bg_r > 200) && (fg_r < 40);
        puts(ok ? "maptint: neutral grays warmed, distinct colors stay distinct, label contrast survives: ok\n" : "maptint: FAILED\n");
        if (!ok) {
            puts("  tg="); puthex(tg); puts(" tw="); puthex(tw);
            puts(" tbg="); puthex(tbg); puts(" tfg="); puthex(tfg); puts("\n");
        }
    }
    else if (!strcmp(line, "walltest")) {
        /* v81 (0.69.0): standing QA per CLAUDE.md's 4b for the wallpaper
           theme picker added this pass. Reverting either gui_map_tint_cool
           or gui_wall_tint's dispatch back to a single-theme shape fails
           this, proved by hand before writing it: temporarily making
           gui_wall_tint always return gui_map_tint(rgb) (the pre-v81
           behaviour) makes check (2) below fail (Cool would equal Warm).
           Real, discriminating checks, same shape as maptinttest above:
           (1) Cool is a genuinely different curve from Warm on the same
               neutral gray -- not just "some different number", but the
               opposite direction: Warm pushes red above blue (r>b), Cool
               pushes blue above red (b>r), on the identical input.
           (2) Cool(gray) != Warm(gray): the two map themes are provably
               distinct, not the same grade under two names.
           (3) Raw(gray) == gray: the "no color grade" theme really is a
               no-op, not a mislabeled copy of one of the tinted paths.
           (4) Cool keeps two distinct source colors distinct after its
               grade (a road gray and a water blue stay != each other),
               proving the contrast-stretch add-on is still a real
               per-pixel transform, not a lerp-to-one-target that would
               collapse them.
           (5) settings_save()/settings_load() round-trip every one of the
               4 real theme values through SETTINGS.TXT (the actual
               Settings-screen persistence path, not a mock), each value
               distinct after reload -- the "config value with no visible
               effect" failure mode this task was scoped to avoid, plus
               the file-format contract wind/dock already get tested at. */
        unsigned int gray  = 0x00A0A0A0;
        unsigned int water = 0x006E9BC7;
        unsigned int warm_g = gui_map_tint(gray);
        unsigned int cool_g = gui_map_tint_cool(gray);
        int wr = (int)((warm_g >> 16) & 0xFF), wb = (int)(warm_g & 0xFF);
        int cr = (int)((cool_g >> 16) & 0xFF), cb = (int)(cool_g & 0xFF);
        int ok1 = (wr > wb) && (cb > cr); /* opposite directions on the same input */
        int ok2 = (cool_g != warm_g);
        int ok4 = (gui_map_tint_cool(gray) != gui_map_tint_cool(water));

        /* ok3 exercises the real per-pixel dispatch (gui_wall_tint), not
           the standalone tint functions above, so a broken dispatch (e.g.
           gui_wall_tint hardcoded back to always-Warm, the pre-v81 shape)
           fails HERE even though ok1/ok2/ok4 would still pass on their
           own -- proved by hand: hardcoding gui_wall_tint to always
           `return gui_map_tint(rgb);` makes wall_dispatch_warm ==
           wall_dispatch_cool below, which this catches. wall_src is
           pointed at a real non-wall_photo address (any distinct
           pointer works, gui_wall_tint only ever compares it, never
           dereferences it here) so the Photo early-out doesn't fire. */
        const unsigned char *saved_wall_src = wall_src;
        int saved_wall_theme = wall_theme;
        unsigned char fake_map_byte = 0;
        wall_src = &fake_map_byte;
        wall_theme = WALL_WARM;  unsigned int wall_dispatch_warm = gui_wall_tint(gray);
        wall_theme = WALL_COOL;  unsigned int wall_dispatch_cool = gui_wall_tint(gray);
        wall_theme = WALL_RAW;   unsigned int wall_dispatch_raw  = gui_wall_tint(gray);
        wall_theme = WALL_SAT;   unsigned int wall_dispatch_sat  = gui_wall_tint(gray);
        unsigned int wall_dispatch_sat_green = gui_wall_tint(0x00208020);
        wall_src = wall_photo; wall_theme = WALL_COOL;
        unsigned int wall_dispatch_photo = gui_wall_tint(gray); /* Photo guard: must ignore wall_theme entirely */
        wall_src = wall_solid_row; wall_theme = WALL_SAT; /* a plain wall is never graded, whatever wall_theme says */
        unsigned int wall_dispatch_solid = gui_wall_tint(gray);
        unsigned int wall_tint_solid = gui_daynight_tint(gray); /* ...and never day/night tinted either */
        wall_src = saved_wall_src; wall_theme = saved_wall_theme;
        int ok3 = (wall_dispatch_warm == warm_g) && (wall_dispatch_cool == cool_g)
                && (wall_dispatch_raw == gray) && (wall_dispatch_photo == gray)
                && (wall_dispatch_sat == gui_sat_color(gray)) && (wall_dispatch_sat != warm_g)
                && (wall_dispatch_solid == gray) && (wall_tint_solid == gray)
                && (wall_solid_color[0] == 0x006E6E6Eu);
        /* Satellite must preserve hue and lift saturation. Returning raw
           pixels or restoring the old luminance-only grade fails here. */
        unsigned int sg = gui_sat_color(0x00208020);
        int ok5 = (gui_sat_color(0x00000000) == 0x00000000)
                && (gui_sat_color(0x00FFFFFF) == 0x00FFFFFF)
                && (((sg >> 8) & 0xFF) > ((sg >> 16) & 0xFF))
                && (((sg >> 8) & 0xFF) > (sg & 0xFF))
                && (((sg >> 8) & 0xFF) - (sg & 0xFF) > 0x60)
                && (wall_dispatch_sat_green == sg);
        ok3 = ok3 && ok5;

        const char *prev_fs = vfs_current_name();
        char prev_fs_buf[16]; int pfi = 0; while (prev_fs[pfi] && pfi < 15) { prev_fs_buf[pfi] = prev_fs[pfi]; pfi++; } prev_fs_buf[pfi] = 0;
        vfs_switch("ramfs");
        int roundtrip_ok = 1;
        for (int theme = WALL_PHOTO; theme <= WALL_INK; theme++) {
            wall_theme = theme; settings_save();
            wall_theme = -1; /* clobber so settings_load has to actually set it, not coast on the old value */
            settings_load();
            if (wall_theme != theme) roundtrip_ok = 0;
        }
        vfs_switch(prev_fs_buf);

        int ok = ok1 && ok2 && ok3 && ok4 && roundtrip_ok;
        puts(ok ? "walltest: Cool distinct from Warm (opposite direction), Raw is a no-op, Cool preserves color distinctness, theme round-trips through SETTINGS.TXT: ok\n" : "walltest: FAILED\n");
        if (!ok) {
            puts("  warm_g="); puthex(warm_g); puts(" cool_g="); puthex(cool_g);
            puts(" dispatch warm="); puthex(wall_dispatch_warm); puts(" cool="); puthex(wall_dispatch_cool);
            puts(" raw="); puthex(wall_dispatch_raw); puts(" photo="); puthex(wall_dispatch_photo);
            puts(" ok1="); putn((unsigned int)ok1); puts(" ok2="); putn((unsigned int)ok2);
            puts(" ok3="); putn((unsigned int)ok3); puts(" ok4="); putn((unsigned int)ok4);
            puts(" roundtrip="); putn((unsigned int)roundtrip_ok); puts("\n");
        }
        wall_theme = WALL_GREY; settings_save(); /* restore the real default, this test must not leave the kernel in a weird state for whatever runs next */
    }
    else return 0;
    return 1;
}
