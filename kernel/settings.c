/* SETTINGS.TXT: the settings that survive a reboot, read once at boot and written on every change, and the LLM, Mail
 and Claude relay settings it carries. Moved out of kernel.c in 4.18.4; kernel/settings.h is what the rest of the
 kernel calls. */
#include "libc.h"
#include "vfs.h"
#include "weather.h"
#include "wallmap.h"
#include "settings.h"

/* v85 (chat rework): global LLM config, the same "one real setting, one
   real default, survives a reboot" contract wind/dock/wall already keep.
   Was hardcoded inline in the shell `chat` command and duplicated again
   in gui_launch_chat (two copies of "llama3.1:8b" / "10.0.2.2" / 11434
   that could silently drift apart); now one source of truth both read.
   1.0.12 (direct owner request, "hook Chat up to our Samantha LLM, the
   Turing project"): the compiled-in default is now the Turing project's
   own Cloudflare Worker (turing.jaybulb.com, plain HTTP port 80,
   model "samantha"), reachable both natively (this file, straight over
   the internet, no VPN/gateway address involved) and from the v86 browser
   demo through worker.js's own `/api/proxy` exception for exactly this
   host+path. Settings (sel == 3/4 below) stays fully editable, same as
   before: anyone who'd rather run a local Ollama server on their own
   machine just points host/port/model back at it. */
char llm_model[LLM_MODEL_MAX] = "samantha";
char llm_host[LLM_HOST_MAX] = "turing.jaybulb.com";
int llm_port = 80;
/* 1.9.26: read-only view for SYS_HTTP_POST (kernel/syscall.c). Settings still owns the write. */
const char *llm_host_get(void) { return llm_host; }
int llm_port_get(void) { return llm_port; }
/* 1.9.27: the Mail token Settings owns (SETTINGS.TXT mailtoken=). Only SYS_HTTP_POST reads it, to
   build the Authorization header for /api/mail/send; no syscall hands it to ring 3. */
char mail_token[MAIL_TOKEN_MAX] = "";
const char *mail_token_get(void) { return mail_token; }
/* 2.14.0: the Claude relay (tools/claude-relay/relay.py) Settings owns: SETTINGS.TXT claudehost=,
   claudeport=, claudetoken=. Its own host, never the LLM host: the token must only ever go to a
   relay the owner pointed at, not to turing.jaybulb.com. Empty host or token means "not set
   up", and SYS_HTTP_POST refuses JT_POST_CLAUDE without touching the network. Only SYS_HTTP_POST
   reads the token, to build the bearer for /api/claude; no syscall hands it to ring 3. */
char claude_host[CLAUDE_HOST_MAX] = "";
int claude_port = 8765;
char claude_token[CLAUDE_TOKEN_MAX] = "";
const char *claude_host_get(void) { return claude_host; }
int claude_port_get(void) { return claude_port; }
const char *claude_token_get(void) { return claude_token; }
/* v85: real chat models actually installed on the host (checked via
   `ollama list`), not a free-text field a typo can point at nothing.
   nomic-embed-text is also installed but is embedding-only, deliberately
   left off. Settings' LLM-model row cycles this list; a stale/hand-edited
   SETTINGS.TXT with anything else falls back to index 0 (samantha) the
   next time the cycle runs, since the cycle only ever writes one of
   these three strings back out.
   v0.85.4 (direct owner request): qwen3:8b promoted to the real default,
   llama3.1:8b kept as the second choice, checked against the same
   `ollama list` on the host, both actually installed.
   1.0.12 (direct owner request): "samantha" (Turing's own Ollama-
   compatible model name, confirmed against its own `/api/chat` reply's
   "model" field) added as the new default, index 0; qwen3:8b and
   llama3.1:8b kept as the local-Ollama alternatives for anyone who sets
   Settings' host back to their own machine. */
const char *LLM_MODELS[LLM_MODEL_COUNT] = { "samantha", "qwen3:8b", "llama3.1:8b" };

/* v47 (0.47.0): settings persisted through the VFS, so "customize the OS
   from inside the OS" actually survives a reboot instead of resetting to
   the compiled-in defaults every boot. Deliberately a flat key=value text
   file (SETTINGS.TXT), not a binary struct: it's human-readable from any
   app that can read a file (cat, the editor), and a corrupt or missing
   file just means defaults, never a crash, since every key is parsed with
   its own bounds check and a real default already set before parsing
   starts.

   v85: grew from int-only values (wind/dock/wall) to also carry two real
   string values (llmmodel/llmhost) plus one more int (llmport), same
   flat key=value shape, just a second value-parsing path alongside the
   existing numeric one rather than a new file format. */
#define SETTINGS_FILE "SETTINGS.TXT"
/* 2.14.0: exact key match for settings_load's newer keys, instead of another letter-by-letter chain. */
int key_is(const char *k, int len, const char *want){
    int i = 0;
    while (i < len && want[i] && k[i] == want[i]) i++;
    return i == len && !want[i];
}
void settings_load(void){
    static char buf[1024]; /* 2.14.0: was 512; the Claude relay's host and token would not fit beside the rest */
    int n = vfs_read_file(SETTINGS_FILE, buf, sizeof(buf) - 1);
    if (n <= 0) return; /* no file yet: compiled-in defaults stand */
    buf[n] = 0;
    for (int i = 0; i < n; ){
        int start = i;
        while (i < n && buf[i] != '\n') i++;
        int line_end = i;
        if (i < n) i++; /* skip the newline */
        int eq = -1;
        for (int j = start; j < line_end; j++) if (buf[j] == '=') { eq = j; break; }
        if (eq < 0) continue;
        int keylen = eq - start;
        int is_wind = keylen == 4 && buf[start]=='w' && buf[start+1]=='i' && buf[start+2]=='n' && buf[start+3]=='d';
        int is_dock = keylen == 4 && buf[start]=='d' && buf[start+1]=='o' && buf[start+2]=='c' && buf[start+3]=='k';
        int is_wall = keylen == 4 && buf[start]=='w' && buf[start+1]=='a' && buf[start+2]=='l' && buf[start+3]=='l';
        int is_llmmodel = keylen == 8 && buf[start]=='l' && buf[start+1]=='l' && buf[start+2]=='m' && buf[start+3]=='m' && buf[start+4]=='o' && buf[start+5]=='d' && buf[start+6]=='e' && buf[start+7]=='l';
        int is_llmhost  = keylen == 7 && buf[start]=='l' && buf[start+1]=='l' && buf[start+2]=='m' && buf[start+3]=='h' && buf[start+4]=='o' && buf[start+5]=='s' && buf[start+6]=='t';
        int is_llmport  = keylen == 7 && buf[start]=='l' && buf[start+1]=='l' && buf[start+2]=='m' && buf[start+3]=='p' && buf[start+4]=='o' && buf[start+5]=='r' && buf[start+6]=='t';
        int is_mailtoken = keylen == 9 && buf[start]=='m' && buf[start+1]=='a' && buf[start+2]=='i' && buf[start+3]=='l' && buf[start+4]=='t' && buf[start+5]=='o' && buf[start+6]=='k' && buf[start+7]=='e' && buf[start+8]=='n';
        int is_loc = keylen == 3 && buf[start]=='l' && buf[start+1]=='o' && buf[start+2]=='c';
        int is_claudehost = key_is(buf + start, keylen, "claudehost");
        int is_claudeport = key_is(buf + start, keylen, "claudeport");
        int is_claudetoken = key_is(buf + start, keylen, "claudetoken");
        if (is_claudehost || is_claudetoken) {
            char *dst = is_claudehost ? claude_host : claude_token;
            int cap = is_claudehost ? CLAUDE_HOST_MAX : CLAUDE_TOKEN_MAX;
            int j = 0, k = eq + 1;
            while (k < line_end && j < cap - 1 && buf[k] > ' ' && buf[k] < 0x7F) dst[j++] = buf[k++];
            dst[j] = 0;
            continue;
        }
        if (is_claudeport) {
            int v = 0, k = eq + 1;
            while (k < line_end && buf[k] >= '0' && buf[k] <= '9' && v < 100000) v = v * 10 + (buf[k++] - '0');
            if (v > 0 && v <= 65535) claude_port = v;
            continue;
        }
        if (is_loc) {
            /* value shape: name;lat;lon -- the same three fields
               loc_geocode fills in, ';'-joined since '=' is already the
               key/value separator and none of the three ever contain a
               ';' (json_extract_string strips escapes, json_extract_number_text
               is digits/./- only). A malformed line (missing a ';', an
               empty lat/lon) is treated as "no override" rather than
               guessed at. */
            int k = eq + 1;
            int f = 0; /* which field: 0=name 1=lat 2=lon */
            char nbuf[LOC_NAME_MAX]; int ni = 0;
            char latbuf[16]; int lai = 0;
            char lonbuf[16]; int loi = 0;
            while (k < line_end) {
                char c = buf[k++];
                if (c == ';') { f++; continue; }
                if (f == 0 && ni < LOC_NAME_MAX - 1) nbuf[ni++] = c;
                else if (f == 1 && lai < 15) latbuf[lai++] = c;
                else if (f == 2 && loi < 15) lonbuf[loi++] = c;
            }
            nbuf[ni] = 0; latbuf[lai] = 0; lonbuf[loi] = 0;
            if (f == 2 && lai > 0 && loi > 0) {
                int j = 0; while (nbuf[j]) { loc_name[j] = nbuf[j]; j++; } loc_name[j] = 0;
                j = 0; while (latbuf[j]) { loc_lat[j] = latbuf[j]; j++; } loc_lat[j] = 0;
                j = 0; while (lonbuf[j]) { loc_lon[j] = lonbuf[j]; j++; } loc_lon[j] = 0;
                loc_have = 1;
                j = 0; while (loc_lat[j]) { geo_lat[j] = loc_lat[j]; j++; } geo_lat[j] = 0;
                j = 0; while (loc_lon[j]) { geo_lon[j] = loc_lon[j]; j++; } geo_lon[j] = 0;
                j = 0; while (loc_name[j] && j < 23) { geo_city[j] = loc_name[j]; j++; } geo_city[j] = 0;
                geo_have = 1;
            }
            continue;
        }
        if (is_llmmodel) {
            char parsed[LLM_MODEL_MAX];
            int j = 0, k = eq + 1;
            while (k < line_end && j < LLM_MODEL_MAX - 1) parsed[j++] = buf[k++];
            parsed[j] = 0;
            /* Validated against the real installed-model list, not
               accepted verbatim: a hand-edited or stale SETTINGS.TXT
               naming a model that isn't one of the two real ones falls
               back to the default (index 0) rather than pointing chat at
               something that will just fail every call, same "can't
               select a theme that doesn't exist" contract wall_theme's
               own clamp already keeps just above. */
            int valid = 0;
            for (int mi = 0; mi < LLM_MODEL_COUNT; mi++) if (!strcmp(parsed, LLM_MODELS[mi])) { valid = 1; break; }
            const char *use = valid ? parsed : LLM_MODELS[0];
            int p = 0; while (use[p] && p < LLM_MODEL_MAX - 1) { llm_model[p] = use[p]; p++; } llm_model[p] = 0;
            continue;
        }
        if (is_mailtoken) {
            int j = 0, k = eq + 1;
            while (k < line_end && j < MAIL_TOKEN_MAX - 1) mail_token[j++] = buf[k++];
            mail_token[j] = 0;
            continue;
        }
        if (is_llmhost) {
            int j = 0, k = eq + 1;
            while (k < line_end && j < LLM_HOST_MAX - 1) llm_host[j++] = buf[k++];
            llm_host[j] = 0;
            if (j == 0) { const char *d = "turing.jaybulb.com"; int p=0; while (d[p]) llm_host[p]=d[p], p++; llm_host[p]=0; }
            continue;
        }
        int val = 0, neg = 0, k = eq + 1;
        if (k < line_end && buf[k] == '-') { neg = 1; k++; }
        while (k < line_end && buf[k] >= '0' && buf[k] <= '9') { val = val * 10 + (buf[k] - '0'); k++; }
        if (neg) val = -val;
        if (is_wind) wind_enabled = (val != 0);
        else if (is_dock && val >= 5 && val <= 25) dock_scale_pct = val;
        else if (is_wall && val >= WALL_PHOTO && val <= WALL_INK) wall_theme = val;
        else if (is_llmport && val > 0 && val <= 65535) llm_port = val;
    }
}

void settings_save(void){
    static char buf[1024]; /* 2.14.0: was 512, see settings_load */
    int n = 0;
    const char *k1 = "wind="; while (*k1) buf[n++] = *k1++;
    buf[n++] = wind_enabled ? '1' : '0'; buf[n++] = '\n';
    const char *k2 = "dock="; while (*k2) buf[n++] = *k2++;
    if (dock_scale_pct >= 10) buf[n++] = '0' + dock_scale_pct / 10;
    buf[n++] = '0' + dock_scale_pct % 10;
    buf[n++] = '\n';
    const char *k3 = "wall="; while (*k3) buf[n++] = *k3++;
    buf[n++] = '0' + wall_theme; buf[n++] = '\n';
    const char *k4 = "llmmodel="; while (*k4) buf[n++] = *k4++;
    { const char *s = llm_model; while (*s && n < (int)sizeof(buf) - 2) buf[n++] = *s++; }
    buf[n++] = '\n';
    const char *k5 = "llmhost="; while (*k5) buf[n++] = *k5++;
    { const char *s = llm_host; while (*s && n < (int)sizeof(buf) - 8) buf[n++] = *s++; }
    buf[n++] = '\n';
    const char *k6 = "llmport="; while (*k6) buf[n++] = *k6++;
    { char digits[8]; int nd = 0; int v = llm_port;
      if (v == 0) digits[nd++] = '0';
      while (v) { digits[nd++] = (char)('0' + v % 10); v /= 10; }
      while (nd) buf[n++] = digits[--nd]; }
    buf[n++] = '\n';
    if (mail_token[0]) {
        const char *k8 = "mailtoken="; while (*k8) buf[n++] = *k8++;
        { const char *s = mail_token; while (*s && n < (int)sizeof(buf) - 80) buf[n++] = *s++; }
        buf[n++] = '\n';
    }
    if (claude_host[0]) {
        const char *k9 = "claudehost="; while (*k9) buf[n++] = *k9++;
        { const char *s = claude_host; while (*s && n < (int)sizeof(buf) - 200) buf[n++] = *s++; }
        buf[n++] = '\n';
        const char *k10 = "claudeport="; while (*k10) buf[n++] = *k10++;
        { char digits[8]; int nd = 0; int v = claude_port;
          if (v == 0) digits[nd++] = '0';
          while (v) { digits[nd++] = (char)('0' + v % 10); v /= 10; }
          while (nd) buf[n++] = digits[--nd]; }
        buf[n++] = '\n';
    }
    if (claude_token[0]) {
        const char *k11 = "claudetoken="; while (*k11) buf[n++] = *k11++;
        { const char *s = claude_token; while (*s && n < (int)sizeof(buf) - 120) buf[n++] = *s++; }
        buf[n++] = '\n';
    }
    if (loc_have) {
        const char *k7 = "loc="; while (*k7) buf[n++] = *k7++;
        { const char *s = loc_name; while (*s && n < (int)sizeof(buf) - 34) buf[n++] = *s++; }
        buf[n++] = ';';
        { const char *s = loc_lat; while (*s && n < (int)sizeof(buf) - 18) buf[n++] = *s++; }
        buf[n++] = ';';
        { const char *s = loc_lon; while (*s && n < (int)sizeof(buf) - 2) buf[n++] = *s++; }
        buf[n++] = '\n';
    }
    vfs_replace_file(SETTINGS_FILE, buf, (unsigned int)n);
}
