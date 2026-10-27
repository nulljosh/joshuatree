/* The Wi-Fi section of Settings (6.23.0), the seventh, after Users. The network the Pi joins and whether it is connected,
   then a card of the networks the start-up scan heard, loudest of each name. Enter (or a click) on the section gives the
   list the keys; Up and Down pick, Enter opens a password field under the card (stars, never the text), Enter there saves
   it through wifi.c's wifi_save (the PMK made on the Pi, JT/WIFI.TXT) for the next start, Esc steps back out. While the
   field is open set_secret keeps the keys off the UART. The UART says `settings wifi pick N NAME` and `settings wifi saved
   NAME` (arm64-wifi-pane-check.py reads them over a made-up scan, since QEMU has no radio). */
unsigned wifi_networks(const char **name, int *rssi, unsigned max);   /* wifi.c */
const char *wifi_network(void);
int wifi_nic_up(void);
int wifi_save(const char *ssid, unsigned sl, const char *pass, unsigned pl);
void accounts_wipe(void *p, unsigned n);
int ask_char(unsigned code);   /* ask.c: the key as a character, shift and all */
#define WF_ROWS 8
static int wf_focus, wf_sel, wf_pw;
static char wf_pass[64], wf_msg[72];
static unsigned wf_pn;
static int wf_secret(void) { return wf_pw; }
static void wf_say(const char *m) { unsigned i = 0; for (; m[i] && i + 1 < sizeof wf_msg; i++) wf_msg[i] = m[i]; wf_msg[i] = 0; }
static void wf_close_pw(void) { accounts_wipe(wf_pass, sizeof wf_pass); wf_pn = 0; wf_pw = 0; }
static void wf_reset(void) { wf_close_pw(); wf_focus = 0; wf_sel = 0; wf_msg[0] = 0; }
static unsigned wf_list(const char **name, int *rssi) { return wifi_networks(name, rssi, WF_ROWS); }
static void wf_paint(int cx, int cy, int wy, int wh) {
    const char *name[WF_ROWS]; int rssi[WF_ROWS]; unsigned n = wf_list(name, rssi);
    char b[80]; b[0] = 0;
    const char *net = wifi_network();
    s_cat(b, net[0] ? "Joins " : "No network chosen"); if (net[0]) s_cat(b, net);
    gui_text(b, cx, cy - 2, SET_INK);
    gui_text(wifi_nic_up() ? "Connected" : "Not connected", cx + SET_CARD_W - gui_text_width(wifi_nic_up() ? "Connected" : "Not connected"), cy - 2, SET_HINT);
    int top = cy + 24;
    if (!n) {
        gui_text("No networks heard at start.", cx, top + 4, SET_HINT);
        gui_text("QEMU and a Pi with no antenna hear none.", cx, top + 24, SET_HINT);
    } else {
        gui_rounded_rect_on_wallpaper(cx, top, SET_CARD_W, (int)n * SET_ROW_H, 0x00FFFFFF, 10);
        for (unsigned r = 0; r < n; r++) {
            int ry = top + (int)r * SET_ROW_H, picked = wf_focus && (int)r == wf_sel;
            if (r) gui_hairline_h(cx + 12, ry, SET_CARD_W - 24, 0x00E8E2D8);
            if (picked) gui_rounded_rect_on_wallpaper(cx + 4, ry + 3, SET_CARD_W - 8, SET_ROW_H - 6, 0x00F3E2D8, 7);
            gui_text(name[r], cx + 14, ry + 7, picked ? SET_ACCENT : SET_INK);
            const char *sig = rssi[r] >= -60 ? "Strong" : rssi[r] >= -72 ? "Okay" : "Weak";
            gui_text(sig, cx + SET_CARD_W - 14 - gui_text_width(sig), ry + 7, SET_HINT);
        }
    }
    int fy = top + (int)(n ? n : 2) * SET_ROW_H + 10;
    if (wf_pw && n) {
        char l[80]; l[0] = 0; s_cat(l, "Password for "); s_cat(l, name[wf_sel]);
        gui_text(l, cx, fy, SET_INK);
        gui_rounded_rect_on_wallpaper(cx, fy + 22, SET_CARD_W, 26, 0x00FFFFFF, 7);
        char st[64]; unsigned k = 0; for (; k < wf_pn && k < 40; k++) st[k] = '*'; st[k] = 0;
        gui_text(st, cx + 10, fy + 26, SET_INK);
        window_rect(cx + 11 + gui_text_width(st), fy + 27, 1, 16, SET_ACCENT);
    }
    gui_text(wf_msg, cx, wy + wh - 52, SET_HINT);
    gui_text(wf_pw ? "Enter saves for the next start, Esc cancels" : wf_focus ? "Up and Down pick, Enter asks the password, Esc" : "Enter picks a network, Esc closes", cx, wy + wh - 32, SET_HINT);
}
static void wf_enter(void) {
    const char *name[WF_ROWS]; int rssi[WF_ROWS];
    if (!wf_list(name, rssi)) { wf_say("No networks to pick."); set_paint(); return; }
    wf_focus = 1; wf_msg[0] = 0;
    if (wf_sel >= WF_ROWS) wf_sel = 0;
    set_log("wifi pick", wf_sel + 1, name[wf_sel]);
    set_paint();
}
static int wf_key(unsigned code) {   /* the list or the password field has the keys */
    const char *name[WF_ROWS]; int rssi[WF_ROWS]; unsigned n = wf_list(name, rssi);
    if (code == 1) { if (wf_pw) wf_close_pw(); else wf_focus = 0; set_paint(); return 1; }
    if (!wf_pw) {
        if ((code == 103 || code == 108) && n) { wf_sel = (wf_sel + (code == 108 ? 1 : (int)n - 1)) % (int)n; set_log("wifi pick", wf_sel + 1, name[wf_sel]); }
        else if ((code == 28 || code == 96) && n) { wf_pw = 1; wf_pn = 0; wf_msg[0] = 0; }
        set_paint(); return 1;
    }
    if (code == 14) { if (wf_pn) wf_pass[--wf_pn] = 0; set_paint(); return 1; }
    if (code == 28 || code == 96) {
        const char *nm = name[wf_sel]; unsigned nl = 0; while (nm[nl]) nl++;
        int r = wifi_save(nm, nl, wf_pass, wf_pn);
        wf_close_pw();
        if (r == 1) { wf_say("Saved. The Pi joins it at its next start."); set_log("wifi saved", -1, nm); wf_focus = 0; }
        else wf_say(r < 0 ? "A password is 8 to 63 plain characters." : "No card to save it on.");
        set_paint(); return 1;
    }
    int ch = ask_char(code);
    if (ch >= 32 && ch < 127 && wf_pn < 63) { wf_pass[wf_pn++] = (char)ch; wf_pass[wf_pn] = 0; set_paint(); }
    return 1;
}
static void wf_click(int lx, int ly, int cx, int cy) {   /* a row picks it and opens the password field */
    const char *name[WF_ROWS]; int rssi[WF_ROWS]; unsigned n = wf_list(name, rssi);
    int top = cy + 24;
    for (unsigned r = 0; r < n; r++) if (lx >= cx && lx < cx + SET_CARD_W && ly >= top + (int)r * SET_ROW_H && ly < top + (int)(r + 1) * SET_ROW_H) {
        wf_focus = 1; wf_sel = (int)r; wf_pw = 1; wf_pn = 0; wf_msg[0] = 0;
        set_log("wifi pick", wf_sel + 1, name[wf_sel]);
        set_paint(); return;
    }
}
