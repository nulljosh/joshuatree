/* The shell's dispatcher, out of kernel.c since 4.20.21: split_argv and run(), the one place a typed line becomes a
   command. Each family of commands lives in its own file and is asked first (selftest.c, shellfs.c, shellnet.c,
   wxtest.c, taskdemo.c, guitest.c, progtest.c); what is left here is the small built-ins. */
#include "libc.h"
#include "serial.h"
#include "console.h"
#include "selftest.h"
#include "kheap.h"
#include "irq.h"
#include "net.h"
#include "klog.h"
#include "bench.h"
#include "picker.h"
#include "sb16.h"
#include "speak.h"
#include "ttf.h"
#include "settings.h"
#include "pmm.h"
#include "task.h"
#include "wallmap.h"
#include "weather.h"
#include "exec.h"
#include "shell.h"

/* Splits `s` in place at runs of spaces into `argv`, the same way every
   argument list in this shell is built: no quotes, no escapes, just
   words, argv[i] pointing back into `s` itself. Shared by `exec` and the
   bare-name fallthrough in run() below so there is one splitter, not two.
   Returns the word count, or -1 if there were more than `max`. */
static int split_argv(char *s, const char **argv, int max) {
    int argc = 0;
    char *p = s;
    while (*p) {
        while (*p == ' ') *p++ = 0;
        if (!*p) break;
        if (argc >= max) return -1;
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
    }
    return argc;
}

static void run(char *line){
    char *arg = line;
    while (*arg && *arg != ' ') arg++;
    if (*arg) *arg++ = 0;

    if (!*line)                    return;
    if (selftest_run(line, arg)) return;   /* kernel/selftest.c: the built-in self-test commands */
    if (taskdemo_run(line)) return;        /* kernel/taskdemo.c: the scheduler demos */
    if (shellfs_run(line, arg)) return;    /* kernel/shellfs.c: ls, cat, rm, cd, mkdir, write, trash, browse */
    if (shellnet_run(line, arg)) return;
    if (wxtest_run(line, arg)) return;
    if (!strcmp(line, "help"))       { puts("help clear echo time uptime dmesg mem reboot crash pagefault bench heaptest heapgrow tasktest fputest preempttest weathertest daynighttest maptinttest walltest weatherfxtest weatherfxcliptest geotest weatherpaneltest windweathertest cursortest texttest wraptest mailtest dockstyletest wind isotest reaptest ring3test usertest notetest filetest ps kill killtest sleep disktest diskuse fsuse ls cat exec rm cd mkdir write browse lspci gfxtest fonttest mousetest nettest ifconfig netscan web serve serveapp chat build gui testapps calctest pngtest jpegtest chattest beep say listen pick pickcheck\n");
                                        puts("a name that isn't one of the above runs a program by that name too, e.g. \"hello\" or \"note buy milk\" (same as exec, case-insensitive)\n"); }
    else if (!strcmp(line, "clear")) clear();
    else if (!strcmp(line, "echo"))  { puts(arg); putc('\n'); }
    else if (!strcmp(line, "crash")) __asm__ volatile ("int $3");  /* manual check: exercises idt/isr */
    else if (!strcmp(line, "pagefault")) { volatile int *p = (int *)0xDEAD0000; *p = 1; } /* manual check: exercises paging */
    else if (!strcmp(line, "bench"))     { bench_run(fs_ok_global); }
    else if (!strcmp(line, "beep"))      puts(sb16_beep(440, 500) ? "beep: played 440Hz\n" : "beep: no sound card\n");
    else if (!strcmp(line, "listen")) {  /* headless test hook for tools/checks/sb16-record-check.py: records 2s at 16kHz, reports bytes captured */
        unsigned int cap = 16000u * 2u;
        unsigned char *buf = kmalloc(cap);
        if (!buf) puts("listen: out of memory\n");
        else {
            int n = sb16_record(buf, cap, 16000u);
            char msg[48]; int p = 0;
            const char *h = "listen: n="; while (*h) msg[p++] = *h++;
            unsigned int v = n < 0 ? 0 : (unsigned int)n;
            unsigned int digs[12]; int dn = 0;
            if (v == 0) msg[p++] = '0';
            while (v) { digs[dn++] = v % 10; v /= 10; }
            while (dn) msg[p++] = (char)('0' + digs[--dn]);
            msg[p++] = '\n'; msg[p] = 0;
            puts(msg);
            kfree(buf);
        }
    }
    else if (!strcmp(line, "say"))       puts(!*arg ? "usage: say <text>\n" : !net_init(0x0A00020F) ? "say: no NIC\n"
                                              : speak_text(llm_host, (unsigned short)llm_port, arg, 1500 /* ~15s at 100Hz */) ? "say: played\n" : "say: nothing played\n");
    else if (!strcmp(line, "pick")) {
        if (!*arg) puts("usage: pick <sentence>\n");
        else { puts("pick: "); puts(picker_pick(arg)); puts("\n"); }
    }
    else if (!strcmp(line, "uptime")){ putn(ticks() / 100); puts("s\n"); }
    else if (!strcmp(line, "fonts")) {
        /* 2.27.0 font registry: every embedded face, by name. */
        for (int i = 0; i < TTF_FACE_COUNT; i++) {
            puts(ttf_face_name((ttf_face_t)i));
            puts(!ttf_face_ok((ttf_face_t)i) ? "  (broken at boot, drawn as DejaVu Sans)\n" : ttf_face_is_mono((ttf_face_t)i) ? "  (mono)\n" : "\n");
        }
    }
    else if (!strcmp(line, "dmesg")) klog_dump();
    else if (!strcmp(line, "mem")) {
        putn(pmm_free_frames() * 4); puts("K free / ");
        putn(pmm_total_frames() * 4); puts("K total (4K frames)\n");
    }
    else if (!strcmp(line, "sleep")) { puts("sleeping 1s...\n"); sleep_ticks(100); puts("awake\n"); }
    else if (!strcmp(line, "ps")) {
        for (int i = 0; i < task_max(); i++) {
            char buf[16]; int n = 0; unsigned int v = (unsigned int)i;
            char tmp[6]; int ti = 0; if (v==0) tmp[ti++]='0'; while (v) { tmp[ti++]='0'+v%10; v/=10; }
            while (ti) buf[n++] = tmp[--ti];
            buf[n++]=' '; buf[n]=0;
            puts(buf);
            puts(task_used(i) ? "used\n" : "free\n");
        }
    }
    else if (!strcmp(line, "kill")) {
        if (!*arg) { puts("usage: kill <task id>, see ps\n"); }
        else {
            int id = 0; const char *p = arg; while (*p >= '0' && *p <= '9') { id = id*10 + (*p-'0'); p++; }
            task_kill(id);
            puts("signal sent (takes effect next time that task is scheduled)\n");
        }
    }
    else if (guitest_run(line, arg)) {}   /* 4.20.2: cursortest, texttest, wraptest and dockstyletest live in kernel/guitest.c */
    else if (!strcmp(line, "wind")) {
        if (!strcmp(arg, "off")) { wind_enabled = 0; settings_save(); puts("wind off\n"); }
        else if (!strcmp(arg, "on")) { wind_enabled = 1; settings_save(); puts("wind on\n"); }
        else { puts(wind_enabled ? "wind is on (wind off to stop)\n" : "wind is off (wind on to start)\n"); }
    }
    else if (!strcmp(line, "weatherfx")) {
        /* v75: force the particle overlay's input (rain/snow/off) without
           waiting for real weather, so tools/wallfx-check.py can prove
           particles composite over a fetched map wallpaper headlessly.
           Sets exactly the fields weather_fetch would, nothing else. */
        if (!strcmp(arg, "rain")) { weather_have = 1; weather_code10 = 610; puts("weatherfx: rain\n"); }
        else if (!strcmp(arg, "snow")) { weather_have = 1; weather_code10 = 710; puts("weatherfx: snow\n"); }
        else if (!strcmp(arg, "off")) { weather_have = 0; puts("weatherfx: off\n"); }
        else puts("usage: weatherfx rain|snow|off\n");
    }
    else if (!strcmp(line, "wallpaper")) {
        /* v75/v81: grey|terracotta|clay|ink (4.11.4, the plain walls)|photo|warm|cool|raw picks the theme (persisted like
           wind/dockscale); "map" stays a working alias for "warm" (v75's
           original two-state name, now one of four themes) so nothing
           that already typed `wallpaper map` breaks. fetch forces the map
           download right now (its own NIC/net bring-up, same as
           weather_fetch), no argument reports state. */
        /* 4.11.4: the four plain walls (the Pi's names and colours). */
        if (!strcmp(arg, "grey") || !strcmp(arg, "gray")) { wall_switch_theme(WALL_GREY); wall_apply(0); puts("wallpaper: grey\n"); }
        else if (!strcmp(arg, "terracotta")) { wall_switch_theme(WALL_TERRA); wall_apply(0); puts("wallpaper: terracotta\n"); }
        else if (!strcmp(arg, "clay")) { wall_switch_theme(WALL_CLAY); wall_apply(0); puts("wallpaper: clay\n"); }
        else if (!strcmp(arg, "ink")) { wall_switch_theme(WALL_INK); wall_apply(0); puts("wallpaper: ink\n"); }
        else if (!strcmp(arg, "photo")) { wall_switch_theme(WALL_PHOTO); wall_apply(0); puts("wallpaper: photo\n"); }
        else if (!strcmp(arg, "map") || !strcmp(arg, "warm")) { wall_switch_theme(WALL_WARM); wall_apply(1); puts(wall_map ? "wallpaper: map (warm)\n" : "wallpaper: map (warm) (fetches on the next weather cycle, or: wallpaper fetch)\n"); }
        else if (!strcmp(arg, "cool")) { wall_switch_theme(WALL_COOL); wall_apply(1); puts(wall_map ? "wallpaper: map (cool)\n" : "wallpaper: map (cool) (fetches on the next weather cycle, or: wallpaper fetch)\n"); }
        else if (!strcmp(arg, "raw")) { wall_switch_theme(WALL_RAW); wall_apply(1); puts(wall_map ? "wallpaper: map (raw)\n" : "wallpaper: map (raw) (fetches on the next weather cycle, or: wallpaper fetch)\n"); }
        else if (!strcmp(arg, "sat") || !strcmp(arg, "satellite")) { wall_switch_theme(WALL_SAT); wall_apply(1); puts(wall_map ? "wallpaper: satellite\n" : "wallpaper: satellite (fetches on the next weather cycle, or: wallpaper fetch)\n"); }
        else if (!strcmp(arg, "fetch")) {
            if (!net_init(0x0A00020F)) { puts("no NIC\n"); }
            else { if (!geo_have) geo_fetch();
                   if (wall_fetch()) { wall_apply(1); puts("wallpaper: map fetched (tiles "); putn((unsigned int)wall_map_tx); puts(","); putn((unsigned int)wall_map_ty); puts(" z"); putn(WALL_ZOOM); puts(")\n"); }
                   else puts("wallpaper: fetch failed, photo stays\n"); }
        }
        else {
            const char *tn = wall_theme >= WALL_GREY ? wall_solid_name[wall_theme - WALL_GREY] : wall_theme == WALL_PHOTO ? "photo" : wall_theme == WALL_COOL ? "map (cool)" : wall_theme == WALL_RAW ? "map (raw)" : wall_theme == WALL_SAT ? "satellite" : "map (warm)";
            puts("wallpaper: "); puts(tn);
            puts(wall_src == wall_photo ? " (showing photo)\n" : wall_src == wall_solid_row ? " (plain)\n" : " (showing map)\n");
        }
    }
    else if (!strcmp(line, "dockscale")) {
        if (!*arg) { puts("dock scale: "); putn((unsigned int)dock_scale_pct); puts("% (dockscale <5-25> to set)\n"); }
        else {
            int v = 0; const char *p = arg; while (*p >= '0' && *p <= '9') { v = v*10 + (*p-'0'); p++; }
            if (v < 5 || v > 25) { puts("usage: dockscale <5-25>\n"); }
            else { dock_scale_pct = v; settings_save(); puts("dock scale set\n"); }
        }
    }
    else if (!strcmp(line, "exec")) {
        if (!*arg) { puts("usage: exec <file> [args...] (a flat binary, run as a real ring-3 task; a bare program name works too, see help)\n"); }
        else {
            /* v2: the words after the filename become the program's real
               argv, argv[0] is the filename itself, and anything past
               JT_ARGC_MAX is refused rather than dropped, because a
               program handed a silently shortened argv has no way to
               tell. There are no quotes and no escapes here; a word is a
               run of non-space characters, which is the whole of what
               this shell's own line reader can express. */
            const char *uargv[JT_ARGC_MAX];
            int uargc = split_argv(arg, uargv, JT_ARGC_MAX);
            if (uargc < 0) { puts("exec: too many arguments (max "); putdec(JT_ARGC_MAX); puts(" including the program name)\n"); }
            else {
                /* 1.0.0: resolved through the same bare-name lookup as
                   the fallthrough below, so `exec hello` and typing
                   `hello` land on the same file. uargv[0] still names
                   the failure if nothing resolves. */
                char resolved[JT_RESOLVE_NAME_MAX];
                if (exec_resolve_name(uargv[0], resolved)) uargv[0] = resolved;
                int status = -1;
                if (!exec_user(uargv[0], uargv, uargc, &status)) { puts(uargv[0]); puts(": exec failed (not found, too big, argv too large, or no free task slot)\n"); }
                else { puts("exit code "); putdec(status); putc('\n'); }
            }
        }
    }
    else if (!strcmp(line, "chat") || !strcmp(line, "samantha")) {
        /* 1.9.26: Samantha is a ring-3 program (user/samantha.c); the text shell opens her window like `notes`. */
        puts("chat: Samantha is a desktop window now, run gui and click her in the dock\n"); /* 2.0 gate 5: no blocking launch from the text shell */
    }
    else if (!strcmp(line, "notes")) {
        puts("notes: Notes is a desktop window now, run gui and click it in the dock\n"); /* 2.0 gate 5: no blocking launch from the text shell */
    }
    else if (!strcmp(line, "gui")) {
        gui_run();
    }
    else if (!strcmp(line, "time"))  show_time();
    else if (!strcmp(line, "reboot"))reboot();
    else {
        /* 1.0.0: before calling `line` an unknown command, try it as a
           program name -- the real shell gap the roadmap calls out.
           `arg` (everything after the first space, already split off
           above) becomes argv[1..] the same way exec's own argv[1..]
           does, through the same split_argv() and exec_user(); this is
           not a second exec path, just a second way to reach the first
           one's name. */
        char resolved[JT_RESOLVE_NAME_MAX];
        if (exec_resolve_name(line, resolved)) {
            const char *pargv[JT_ARGC_MAX];
            pargv[0] = resolved;
            int rest = split_argv(arg, pargv + 1, JT_ARGC_MAX - 1);
            if (rest < 0) { puts("exec: too many arguments (max "); putdec(JT_ARGC_MAX); puts(" including the program name)\n"); }
            else {
                int status = -1;
                if (!exec_user(resolved, pargv, rest + 1, &status)) { puts(resolved); puts(": exec failed (not found, too big, argv too large, or no free task slot)\n"); }
                else { puts("exit code "); putdec(status); putc('\n'); }
            }
        }
        else { puts("? "); puts(line); putc('\n'); }
    }
}

void shell_line(char *line) { run(line); }   /* kmain's line reader and kernel/progtest.c's notetest */
