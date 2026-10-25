/* The kernel's dmesg ring buffer and the notification panel that reads it, out of kernel.c since 4.20.6. */
#include "libc.h"
#include "console.h"
#include "irq.h"
#include "serial.h"
#include "pmm.h"
#include "trash.h"
#include "desktop.h"
#include "window.h"
#include "font.h"
#include "gui_paint.h"
#include "selftest.h"
#include "klog.h"

/* A real, Linux-style dmesg ring buffer: fixed-size, overwrites its
   oldest entry once full rather than growing, since this kernel has no
   log rotation and a boot-time trace doesn't need one. Each entry is
   tagged with the real PIT tick count (irq.c's ticks()) at the moment it
   was logged, the closest thing to a timestamp available this early;
   anything logged before irq_install() has actually run and interrupts
   are enabled reads tick 0, an honest "no timer yet", not a bug. */
#define KLOG_MAX     32
#define KLOG_MSG_LEN 60
static char klog_buf[KLOG_MAX][KLOG_MSG_LEN];
static unsigned int klog_tick[KLOG_MAX];
static int klog_count = 0, klog_next = 0;

void klog(const char *msg){
    int i = 0;
    while (msg[i] && i < KLOG_MSG_LEN - 1) { klog_buf[klog_next][i] = msg[i]; i++; }
    klog_buf[klog_next][i] = 0;
    klog_tick[klog_next] = ticks();
    klog_next = (klog_next + 1) % KLOG_MAX;
    if (klog_count < KLOG_MAX) klog_count++;
    serial_puts(msg); serial_puts("\n"); /* live trace, survives a crash the ring buffer's own reset wouldn't */
}

void klog_dump(void){
    int start = (klog_count < KLOG_MAX) ? 0 : klog_next;
    for (int i = 0; i < klog_count; i++){
        int idx = (start + i) % KLOG_MAX;
        puts("[ "); putn(klog_tick[idx]); puts("] "); puts(klog_buf[idx]); putc('\n');
    }
}

/* v42: the clock opens a notification panel, the way clicking the clock
   on a Mac does. Honest about what "notifications" means on a kernel with
   no apps posting any: it's the system's own recent log (the same ring
   buffer `dmesg` prints) plus live warnings computed right now (memory
   running low, trash nearly full). Real state, not placeholder cards. */
#define NOTIF_W     360
#define NOTIF_ROWS  8
/* v0.76.20: the notif panel used to echo klog's raw boot-log lines verbatim
   ("vmmouse_init: VMware backdoor answered, absolute pointer on"), which
   reads like `dmesg`, not Notification Center. Translate to plain text here,
   at render time only -- klog_buf itself is untouched, so vmmouse-check.sh's
   grep against the stored strings still passes. Falls back to the raw line
   for anything not in the table, so a message added later is never dropped. */
static const char *notif_friendly(const char *raw){
    if (web_starts_with(raw, "vmmouse_init: VMware backdoor answered")) return "Mouse: precise tracking on";
    if (web_starts_with(raw, "vmmouse_init: no backdoor")) return "Mouse connected";
    if (web_starts_with(raw, "font_init:")) return "Fonts loaded";
    if (web_starts_with(raw, "pmm_init:")) return "Memory initialized";
    if (web_starts_with(raw, "paging_install:")) return "Memory protection enabled";
    if (web_starts_with(raw, "tasks_init:")) return "Task scheduler ready";
    if (web_starts_with(raw, "fat_mount: FAT16")) return "Disk mounted";
    if (web_starts_with(raw, "fat_mount:")) return "No disk found";
    if (web_starts_with(raw, "vfs: fat + ramfs")) return "Storage ready";
    if (web_starts_with(raw, "vfs: no FAT disk")) return "Using built-in storage";
    if (web_starts_with(raw, "vga_text_mode_init:")) return "Display initialized";
    if (web_starts_with(raw, "gdt_install:")) return "System tables loaded";
    if (web_starts_with(raw, "idt_install:")) return "Interrupts configured";
    if (web_starts_with(raw, "syscall_install:")) return "System calls ready";
    if (web_starts_with(raw, "irq_install:")) return "Timers and input ready";
    if (web_starts_with(raw, "mouse_init:")) return "Mouse connected";
    return raw;
}

void gui_draw_notif_panel(void){
    int x0 = (int)window_width() - NOTIF_W - 4, y0 = GUI_MENUBAR_H;
    unsigned int bg = 0x002C2C2E, text = 0x00F5F5F7, warn = 0x00FFB454;

    /* always-visible memory bar: total vs used (each frame = 4K) */
    unsigned int total_k = pmm_total_frames() * 4;
    unsigned int free_k = pmm_free_frames() * 4;
    unsigned int used_k = total_k > free_k ? total_k - free_k : 0;
    unsigned int usage_percent = total_k > 0 ? (used_k * 100) / total_k : 0;
    unsigned int mem_bar_color = usage_percent > 80 ? warn : text;

    /* live warnings: other than memory, which is always shown */
    const char *warns[2]; int nw = 0;
    if (trash_count() >= TRASH_MAX_ITEMS - 1) warns[nw++] = "Trash is nearly full";
    if (nw == 0) warns[nw++] = "No warnings";

    int n = klog_count < NOTIF_ROWS ? klog_count : NOTIF_ROWS;
    /* height: 10 (margin) + 18 (memory text) + 14 (memory bar) + 18 (nw warnings) + 18 (separator) + n*34 (log rows) + 8 (bottom margin) */
    int total_h = 10 + 18 + 14 + (nw + 1) * 18 + n * 34 + 8;
    gui_rounded_rect_on_wallpaper(x0, y0, NOTIF_W, total_h, bg, GUI_FLYOUT_RADIUS);

    int y = y0 + 8;

    /* memory bar section: text + visual bar */
    char mem_str[40]; int p = 0;
    if (used_k >= 1024) {
        unsigned int used_mb = used_k / 1024;
        unsigned int total_mb = total_k / 1024;
        /* format: "XXXM / XXXM" */
        { unsigned int v = used_mb; char tb[8]; int ti = 0;
          if (!v) tb[ti++] = '0'; while (v) { tb[ti++] = '0' + v % 10; v /= 10; }
          while (ti) mem_str[p++] = tb[--ti]; }
        mem_str[p++] = 'M'; mem_str[p++] = ' '; mem_str[p++] = '/'; mem_str[p++] = ' ';
        { unsigned int v = total_mb; char tb[8]; int ti = 0;
          if (!v) tb[ti++] = '0'; while (v) { tb[ti++] = '0' + v % 10; v /= 10; }
          while (ti) mem_str[p++] = tb[--ti]; }
        mem_str[p++] = 'M';
    } else {
        /* format: "XXXK / XXXK" */
        { unsigned int v = used_k; char tb[8]; int ti = 0;
          if (!v) tb[ti++] = '0'; while (v) { tb[ti++] = '0' + v % 10; v /= 10; }
          while (ti) mem_str[p++] = tb[--ti]; }
        mem_str[p++] = 'K'; mem_str[p++] = ' '; mem_str[p++] = '/'; mem_str[p++] = ' ';
        { unsigned int v = total_k; char tb[8]; int ti = 0;
          if (!v) tb[ti++] = '0'; while (v) { tb[ti++] = '0' + v % 10; v /= 10; }
          while (ti) mem_str[p++] = tb[--ti]; }
        mem_str[p++] = 'K';
    }
    mem_str[p++] = ' '; mem_str[p++] = 'u'; mem_str[p++] = 's'; mem_str[p++] = 'e'; mem_str[p++] = 'd';
    /* v0.76.49: direct request, "more memory information" -- percent used
       alongside the raw M/K figures already shown, same buffer (still well
       under its 40-byte size at max: "999M / 999M used (100%)" is 24). */
    mem_str[p++] = ' '; mem_str[p++] = '(';
    { unsigned int v = usage_percent; char tb[4]; int ti = 0;
      if (!v) tb[ti++] = '0'; while (v) { tb[ti++] = '0' + v % 10; v /= 10; }
      while (ti) mem_str[p++] = tb[--ti]; }
    mem_str[p++] = '%'; mem_str[p++] = ')';
    mem_str[p] = 0;
    font_draw_string(mem_str, x0 + 12, y, mem_bar_color, -1); y += 18;

    /* visual memory bar: full width bar with filled portion */
    int bar_x = x0 + 12, bar_y = y, bar_w = NOTIF_W - 24, bar_h = 6;
    window_rect(bar_x, bar_y, bar_w, bar_h, 0x00545458);  /* background */
    int filled_w = bar_w > 0 ? (bar_w * usage_percent) / 100 : 0;
    if (filled_w > 0) window_rect(bar_x, bar_y, filled_w, bar_h, mem_bar_color);  /* filled portion */
    y += 14;

    window_rect(x0 + 8, y, NOTIF_W - 16, 1, 0x00545458); y += 18;

    /* newest last, like every log ever, capped to the last NOTIF_ROWS */
    int start = (klog_count < KLOG_MAX) ? 0 : klog_next;
    int skip = klog_count - n;
    for (int i = 0; i < n; i++, y += 34) {
        int idx = (start + skip + i) % KLOG_MAX;
        const char *msg = notif_friendly(klog_buf[idx]);
        char line[44]; int p = 0;
        unsigned int t = klog_tick[idx] / 100; char tb[8]; int ti = 0;
        if (!t) tb[ti++] = '0'; while (t) { tb[ti++] = '0' + t % 10; t /= 10; }
        while (ti) line[p++] = tb[--ti];
        line[p++] = 's'; line[p++] = ' ';
        int k = 0;
        for (; msg[k] && p < 42; k++) line[p++] = msg[k];
        line[p] = 0;
        font_draw_string(line, x0 + 12, y, text, -1);
        if (msg[k]) {
            p = 0;
            line[p++] = ' '; line[p++] = ' '; line[p++] = ' ';
            for (; msg[k] && p < 42; k++) line[p++] = msg[k];
            line[p] = 0;
            font_draw_string(line, x0 + 12, y + 16, text, -1);
        }
    }
}
