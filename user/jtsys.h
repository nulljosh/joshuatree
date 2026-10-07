#ifndef JTSYS_H
#define JTSYS_H
/* Joshua Tree user-space syscall wrappers. This is the whole of what a
   ring-3 program gets: no libc, no startup files, no relocations. Every
   function here is one `int $0x80` with the arguments already in the
   registers docs/SYSCALL-ABI.md names, so the generated code is the ABI
   itself rather than a layer over it.

   eax = call number, ebx/ecx/edx = arguments 1-3, eax = result on return,
   negative values are -errno. "memory" is in the clobber list because the
   kernel may write through a pointer argument (read, time), so the
   compiler must not keep a stale copy of that memory in a register across
   the trap. ebx is a general register here (this is built -fno-pic, so it
   is not reserved for a GOT pointer) and can be an input constraint
   directly.

   v2 adds lseek and the open() flags that make write() reach a file, and
   it adds arguments. A program is entered with the i386 cdecl frame a
   plain C function expects, which means this is a valid entry point and
   needs no assembly:

       void _start(int argc, char **argv) { ... jt_exit(0); }

   argv[0] is the program's own name and argv[argc] is NULL. A program
   that wants nothing from its arguments can still declare
   `void _start(void)`, which is what user/hello.c does and why it did not
   have to change. Either way _start must never return: there is no caller
   above it, and the return address the loader leaves at 0(%esp) is 0, so
   returning faults and the kernel reaps the task. Call jt_exit(). */

#define JT_SYS_EXIT        1
#define JT_SYS_READ        3
#define JT_SYS_WRITE       4
#define JT_SYS_OPEN        5
#define JT_SYS_CLOSE       6
#define JT_SYS_TIME        13
#define JT_SYS_LSEEK       19
#define JT_SYS_GETPID      20
#define JT_SYS_SCHED_YIELD 158

/* v3 (1.7.7): windows. Joshua Tree's own numbers, 384 up, past anything
   Linux i386 assigns. A program launched from the dock asks for the app
   window the desktop already opened for it and gets a framebuffer of that
   size to draw into; poll hands back keys, clicks and wheel ticks, and
   with JT_POLL_PRESENT copies the framebuffer to the screen first. There
   is no close call: exiting (or crashing) releases the window. */
#define JT_SYS_WINDOW_OPEN 384
#define JT_SYS_WINDOW_POLL 385
#define JT_SYS_TASKS       386 /* 1.9.6: uptime, memory, live slots; optional kill */
#define JT_SYS_HTTP_GET    387 /* 1.9.11: one GET from joshuatree.heyitsmejosh.com; path only, see kernel/syscall.h */
#define JT_HTTP_PATH_MAX 128   /* longest path SYS_HTTP_GET accepts, bytes before the NUL */
#define JT_HTTP_BODY_MAX 2048  /* longest body bounced and copied out on a 200 */
#define JT_HTTP_BIG_MAX  65536 /* longest len accepted: above 2KB the body lands straight in the caller buffer, scratch bytes possible on failure */
#define JT_SYS_HTTP_POST   392 /* 1.9.26: Samantha's chat request, path only, host stays Settings-owned */
#define JT_SYS_READDIR     388 /* 1.9.13: list a directory into fixed-size records (387 is Curbfind's http_get) */
#define JT_SYS_MKDIR       389 /* make a folder; relative path like open */
#define JT_SYS_UNLINK      390 /* delete a file; relative path like open */
#define JT_SYS_SHELL_RUN   391 /* 1.9.24: run one allowlisted shell line, output into a buffer; see kernel/shellsys.c */
#define JT_SHELL_LINE_MAX 160  /* longest line the kernel copies in, bytes before the NUL: "<cwd>\n<command>" */
#define JT_SYS_AUDIO       393 /* 1.9.26: queue 8-bit unsigned mono PCM / query playback / stop; see kernel/syscall.h */
#define JT_AUDIO_PLAY   1
#define JT_AUDIO_STATUS 2
#define JT_AUDIO_STOP   3
#define JT_AUDIO_END    1  /* flags: this call carries the last bytes of the clip, start now */
#define JT_AUDIO_CHUNK_MAX 8192
#define JT_SYS_AUDIO_RECORD 394 /* 1.9.26: push-to-talk capture, 8-bit unsigned mono; see kernel/syscall.h */
#define JT_REC_START 1
#define JT_REC_READ  2
#define JT_REC_STOP  3
#define JT_REC_CHUNK_MAX 8192
#define JT_SYS_SYSINFO     395 /* 1.9.26: weather, chat host, phone flag, time in one read-only struct; see kernel/syscall.h */
#define JT_SYS_BRK         397 /* 1.9.27: per-task heap top; 0 queries, else sets. Returns the top or -errno; see kernel/brk.c */
#define JT_SYS_CLIPBOARD   399 /* 1.9.28: the one system clipboard, 4 KB; see kernel/syscall.h */
#define JT_CLIP_SET        1
#define JT_CLIP_GET        2
#define JT_CLIP_MAX        4096
#define JT_SYS_LAUNCH_REQUEST 396 /* 1.9.26: ask the desktop to open an app by exact APPS[] name; never launches inside the gate */
#define JT_SYS_REFRESH     398 /* ask the desktop to refetch weather or stocks; poll jt_sysinfo.data_stamp; see kernel/syscall.h */
#define JT_REFRESH_WEATHER 0
#define JT_REFRESH_STOCKS  1
#define JT_SYSINFO_VERSION 1
#define JT_WX_TEXT_MAX  24
#define JT_SYSINFO_HOST_MAX 40
#define JT_APP_NAME_MAX 24
struct jt_sysinfo {
    unsigned int version, size, phone, epoch, wx_have, wx_state;
    int wx_temp_c, wx_code10;
    unsigned int llm_port;
    char wx_text[JT_WX_TEXT_MAX];
    char llm_host[JT_SYSINFO_HOST_MAX];
    unsigned int data_stamp;
};
#define JT_SYS_READFILE    401 /* a whole file into one buffer, up to 6MB (open() stops at 8KB); see kernel/syscall.h */
#define JT_SYS_TEXT        400 /* anti-aliased text for the window (389 is mkdir); see kernel/syscall.h */
#define JT_TEXT_DRAW    0
#define JT_TEXT_MEASURE 1
#define JT_TEXT_CLEAR   2
#define JT_TEXT_MAX     96
struct jt_text { int x, y; unsigned int fg; const char *s; };
#define JT_DIRENT_NAME  32
#define JT_READDIR_MAX  64
#define JT_PATH_MAX     63     /* bytes of path before the NUL the kernel will read; longer is -EINVAL */
#define JT_POLL_PRESENT 1
#define JT_EV_KEY   1
#define JT_EV_CLICK 2
#define JT_EV_WHEEL 3
#define JT_EV_RESIZE 4
/* Key codes above ASCII, the same values the desktop's own apps see. */
#define JT_KEY_UP    256
#define JT_KEY_DOWN  257
#define JT_KEY_ENTER 258
#define JT_KEY_COPY  302 /* Ctrl+C, X, V: the clipboard keys, see jt_clip_set / jt_clip_get */
#define JT_KEY_CUT   303
#define JT_KEY_PASTE 304
#define JT_KEY_ESC   259
#define JT_KEY_LEFT  261
#define JT_KEY_RIGHT 262
#define JT_KEY_HOME  305
#define JT_KEY_END   306
#define JT_KEY_DELETE 307
#define JT_KEY_SAVE  308 /* Ctrl+S */
#define JT_KEY_F2    309 /* F2 pressed: push-to-talk down */
#define JT_KEY_F2_UP 310 /* F2 released */
#define JT_KEY_SLEFT 311 /* Shift+arrow extends a selection, Ctrl+A selects all */
#define JT_KEY_SRIGHT 312
#define JT_KEY_SUP   313
#define JT_KEY_SDOWN 314
#define JT_KEY_SELALL 315
struct jt_window_info { unsigned int width, height, pitch; unsigned int *pixels; };
struct jt_event { unsigned int kind; int a, b; };
struct jt_tasks { unsigned int ticks, free_kb, total_kb, current, used; };
/* SYS_READDIR: one record per entry. The name is NUL-terminated and cut to
   fit, size is 0 for a directory, is_dir is 1 or 0. The path is relative
   to the shell's current directory ("" or "." is that directory itself,
   "DOCS/SUB" two levels down, no leading slash, no "." or ".." parts); the
   kernel walks it and walks back inside the call, so a program keeps its
   own cwd string and passes it every time. The call returns the number of
   entries the directory holds, which can be more than the records it was
   handed: only the first `max` are written. SYS_OPEN takes the same
   relative paths. */
struct jt_dirent { char name[JT_DIRENT_NAME]; unsigned int size, is_dir; };

/* v2 open() flags and lseek() whence values, Linux i386's own numbers.
   O_RDONLY is 0, which is exactly what v1 required, so a v1 program's
   open(path, 0) means the same thing it always did. O_CREAT, O_TRUNC and
   O_APPEND only work alongside O_WRONLY or O_RDWR; asking for one on a
   read-only descriptor is -EINVAL, because nothing is ever written back
   from a read-only descriptor and a silent no-op would be worse. */
#define JT_O_RDONLY  0x0000
#define JT_O_WRONLY  0x0001
#define JT_O_RDWR    0x0002
#define JT_O_CREAT   0x0040
#define JT_O_TRUNC   0x0200
#define JT_O_APPEND  0x0400

#define JT_SEEK_SET 0
#define JT_SEEK_CUR 1
#define JT_SEEK_END 2

/* Flat user binaries get no .bss (see user/hello.ld): nothing zeroes it
   and there is no crt0 to do it at startup, so libjt pins its
   zero-initialised globals into .data instead, which the linker script
   does write out. ELF is the only object format the real i386 target
   uses; a host build (tools/checks/libjt-host-check.sh, on macOS/Mach-O
   here) doesn't have a .bss restriction at all and Mach-O's section
   attribute syntax differs, so this is a no-op there. */
#if defined(__ELF__)
#define JT_DATA __attribute__((section(".data")))
#else
#define JT_DATA
#endif

#if defined(__i386__)
static inline int jt_syscall(int n, unsigned a, unsigned b, unsigned c) {
    int r;
    __asm__ volatile ("int $0x80"
                      : "=a"(r)
                      : "a"(n), "b"(a), "c"(b), "d"(c)
                      : "memory");
    return r;
}
#else
/* Host builds (tools/checks/libjt-host-check.sh) compile libjt's
   string.c/stdlib.c natively to diff them against the host libc; `int
   $0x80` is not valid on a non-x86 host, and nothing the host harness
   exercises actually needs a real trap, so this stands in for it. It is
   never linked into a ring-3 program: every real build of user/ passes
   -target i386-unknown-none, which takes the branch above. */
static inline int jt_syscall(int n, unsigned a, unsigned b, unsigned c) {
    (void)n; (void)a; (void)b; (void)c;
    return -1;
}
#endif

static inline void jt_exit(int code) {
    jt_syscall(JT_SYS_EXIT, (unsigned)code, 0, 0);
    for (;;) { } /* SYS_EXIT never returns; the loop only tells the compiler that too */
}
static inline int jt_read(int fd, void *buf, unsigned len)        { return jt_syscall(JT_SYS_READ,  (unsigned)fd, (unsigned)buf, len); }
static inline int jt_write(int fd, const void *buf, unsigned len) { return jt_syscall(JT_SYS_WRITE, (unsigned)fd, (unsigned)buf, len); }
static inline int jt_open(const char *path, int flags)            { return jt_syscall(JT_SYS_OPEN,  (unsigned)path, (unsigned)flags, 0); }
/* Whole file into buf (at most max bytes): the byte count, 0 for an empty file, -ENOENT, -EFBIG (too big, nothing copied), -EFAULT. */
static inline int jt_close(int fd)                                { return jt_syscall(JT_SYS_CLOSE, (unsigned)fd, 0, 0); }
static inline int jt_time(unsigned *out)                          { return jt_syscall(JT_SYS_TIME,  (unsigned)out, 0, 0); }
static inline int jt_brk(unsigned top)                            { return jt_syscall(JT_SYS_BRK, top, 0, 0); }
static inline int jt_getpid(void)                                 { return jt_syscall(JT_SYS_GETPID, 0, 0, 0); }
static inline int jt_sched_yield(void)                            { return jt_syscall(JT_SYS_SCHED_YIELD, 0, 0, 0); }
static inline int jt_lseek(int fd, int off, int whence)           { return jt_syscall(JT_SYS_LSEEK, (unsigned)fd, (unsigned)off, (unsigned)whence); }
static inline int jt_window_open(struct jt_window_info *info)     { return jt_syscall(JT_SYS_WINDOW_OPEN, (unsigned)info, 0, 0); }
static inline int jt_tasks(struct jt_tasks *t, int kill)          { return jt_syscall(JT_SYS_TASKS, (unsigned)t, (unsigned)kill, 0); }
/* Body bytes on HTTP 200 (at most len), minus the status on any other reply
   (-100..-599), or -errno: -EINVAL bad path, -EFAULT bad pointer, -ENODEV no
   NIC, -EIO no answer. The host is fixed in the kernel; only the path is ours. */
static inline int jt_http_get(const char *path, void *buf, unsigned len) { return jt_syscall(JT_SYS_HTTP_GET, (unsigned)path, (unsigned)buf, len); }
/* One POST of application/json to the chat host. Body at most 6144 bytes, out clamped to 8192,
   reply_ticks 0 = 15s, clamped to 45s. Reply body bytes on HTTP 200, -status on any other reply,
   or -errno (-EBUSY a fetch is already in flight). */
struct jt_http_post { const char *path; const char *body; unsigned int body_len; char *out; unsigned int out_len; unsigned int reply_ticks; };
static inline int jt_http_post(struct jt_http_post *a) { return jt_syscall(JT_SYS_HTTP_POST, (unsigned)a, 0, 0); }
#define JT_POST_WORKER 1u /* post to the fixed joshuatree Worker host instead of the chat host */
#define JT_POST_BIG    2u /* body and reply up to JT_HTTP_BIG_MAX, straight from/to the caller buffers */
#define JT_POST_CLAUDE 4u /* 2.14.0: to the Settings-owned Claude relay, path /api/claude only; the kernel adds the token. -13 (EACCES) when no relay is set */
#define JT_HTTP_POST_TICKS_CLAUDE 24000 /* the longer wait JT_POST_CLAUDE allows */
static inline int jt_http_post_ex(struct jt_http_post *a, unsigned flags) { return jt_syscall(JT_SYS_HTTP_POST, (unsigned)a, flags, 0); }
static inline int jt_readdir(const char *path, struct jt_dirent *out, unsigned max) { return jt_syscall(JT_SYS_READDIR, (unsigned)path, (unsigned)out, max); }
/* Bytes read (at most cap), -ENOENT missing or empty, -EINVAL, -EFAULT. */
static inline int jt_readfile(const char *path, void *buf, unsigned cap) { return jt_syscall(JT_SYS_READFILE, (unsigned)path, (unsigned)buf, cap); }
static inline int jt_mkdir(const char *path)                      { return jt_syscall(JT_SYS_MKDIR, (unsigned)path, 0, 0); }
static inline int jt_unlink(const char *path)                     { return jt_syscall(JT_SYS_UNLINK, (unsigned)path, 0, 0); }
static inline int jt_shell_run(const char *line, char *out, unsigned outlen) { return jt_syscall(JT_SYS_SHELL_RUN, (unsigned)line, (unsigned)out, outlen); }
/* Fills at most size bytes (pass sizeof *si); returns bytes written, -EINVAL under 8, -EFAULT bad pointer.
   si->version is first so the struct can grow; check si->size for what the kernel really wrote. */
static inline int jt_sysinfo(struct jt_sysinfo *si)               { return jt_syscall(JT_SYS_SYSINFO, (unsigned)si, sizeof *si, 0); }
/* 0 queued, -EINVAL no app by that exact name, -EBUSY one is already pending. The desktop opens it on its next pass. */
static inline int jt_refresh(int kind, int arg)                   { return jt_syscall(JT_SYS_REFRESH, (unsigned)kind, (unsigned)arg, 0); }
static inline int jt_launch(const char *app)                      { return jt_syscall(JT_SYS_LAUNCH_REQUEST, (unsigned)app, 0, 0); }
/* Replace the system clipboard with len bytes (0 clears). Returns len, -EINVAL over JT_CLIP_MAX, -EFAULT. */
static inline int jt_clip_set(const void *buf, unsigned len)      { return jt_syscall(JT_SYS_CLIPBOARD, JT_CLIP_SET, (unsigned)buf, len); }
/* Copy out at most len bytes of the clipboard; returns bytes copied (0 when empty), -EFAULT. A short len clips the paste. */
static inline int jt_clip_get(void *buf, unsigned len)            { return jt_syscall(JT_SYS_CLIPBOARD, JT_CLIP_GET, (unsigned)buf, len); }
struct jt_audio_play { const void *pcm; unsigned int len; unsigned int rate; unsigned int flags; };
struct jt_audio_status { unsigned int version, size, playing, queued, space, rate, played; };
/* Queues up to JT_AUDIO_CHUNK_MAX bytes of 8-bit unsigned mono PCM at rate Hz. Returns bytes taken
   (0 = ring full, retry next frame), -ENODEV no card. Pass JT_AUDIO_END on the call that carries
   the clip's last bytes; if a chunk is cut short the flag is dropped, so resend it with the rest. */
static inline int jt_audio_play(const void *pcm, unsigned len, unsigned rate, unsigned flags) {
    struct jt_audio_play p = { pcm, len, rate, flags };
    return jt_syscall(JT_SYS_AUDIO, JT_AUDIO_PLAY, (unsigned)&p, sizeof p);
}
/* played is in samples: mouth time in ms = played * 1000 / rate. */
static inline int jt_audio_status(struct jt_audio_status *st)    { return jt_syscall(JT_SYS_AUDIO, JT_AUDIO_STATUS, (unsigned)st, sizeof *st); }
static inline int jt_audio_stop(void)                            { return jt_syscall(JT_SYS_AUDIO, JT_AUDIO_STOP, 0, 0); }
/* Arms capture at rate Hz: 0, -ENODEV no card, -EBUSY playback owns it (or the last take is still landing; retry). */
static inline int jt_rec_start(unsigned rate)                    { return jt_syscall(JT_SYS_AUDIO_RECORD, JT_REC_START, rate, 0); }
/* Copies up to n (max JT_REC_CHUNK_MAX) banked bytes, oldest first; returns the count, 0 = none yet. Never blocks. */
static inline int jt_rec_read(void *buf, unsigned n)             { return jt_syscall(JT_SYS_AUDIO_RECORD, JT_REC_READ, (unsigned)buf, n); }
static inline int jt_rec_stop(void)                              { return jt_syscall(JT_SYS_AUDIO_RECORD, JT_REC_STOP, 0, 0); }
static inline int jt_text(const char *s, int x, int y, unsigned fg, int op) { struct jt_text t = { x, y, fg, s }; return jt_syscall(JT_SYS_TEXT, (unsigned)&t, (unsigned)op, 0); }
static inline void jt_text_clear(void) { jt_syscall(JT_SYS_TEXT, 0, JT_TEXT_CLEAR, 0); }
/* n bytes of s as one SYS_TEXT run (op DRAW or MEASURE); the width, or -1. */
static inline int jt_text_n(const char *s, int n, int x, int y, unsigned fg, int op) {
    char b[JT_TEXT_MAX + 1];
    if (n > JT_TEXT_MAX) n = JT_TEXT_MAX;
    for (int i = 0; i < n; i++) b[i] = s[i];
    b[n] = 0;
    return jt_text(b, x, y, fg, op);
}
/* Greedy word wrap with the real advance. Breaks only at spaces (a word
   wider than the column gets its own line). Lines are line_h apart, at most
   max_lines of them; returns the lines used. fg 0xFFFFFFFF measures only. */
static inline int jt_wrap(const char *s, int x, int y, int max_w, int line_h, int max_lines, unsigned fg) {
    int lines = 0;
    while (*s == ' ') s++;
    while (*s && lines < max_lines) {
        int end = 0, e = 0;                  /* end: bytes on this line so far */
        for (;;) {
            int wl = 0;
            while (s[e + wl] && s[e + wl] != ' ') wl++;
            if (!wl) break;
            int w = jt_text_n(s, e + wl, 0, 0, 0, JT_TEXT_MEASURE);
            if (w < 0) w = (e + wl) * 8;
            if (end && w > max_w) break;
            end = e + wl; e = end;
            while (s[e] == ' ') e++;
            if (!s[e]) break;
        }
        if (!end) break;
        if (end > JT_TEXT_MAX) end = JT_TEXT_MAX;
        jt_text_n(s, end, x, y + lines * line_h, fg, JT_TEXT_DRAW);
        lines++;
        s += end;
        while (*s == ' ') s++;
    }
    return lines;
}
static inline int jt_window_poll(struct jt_event *ev, unsigned flags) { return jt_syscall(JT_SYS_WINDOW_POLL, (unsigned)ev, flags, 0); }
/* Resize helper, the app's two lines: after a poll, `if (r == 1 && jt_window_resized(&ev, &win)) { draw(); flags = JT_POLL_PRESENT; continue; }`.
   Returns 1 when ev was JT_EV_RESIZE and re-opening the window gave *info a buffer
   of the new size (the app must redraw all of it: the new buffer starts zeroed), else 0.
   Logs "ring3: window now WxH" once the new size is mapped, the line ring3resize-check.py reads. */
static inline int jt_window_resized(const struct jt_event *ev, struct jt_window_info *info) {
    if (ev->kind != JT_EV_RESIZE) return 0;
    struct jt_window_info n;
    if (jt_window_open(&n) != 0 || !n.pixels) return 0;
    *info = n;
    char b[40]; int l = 0; const char *a = "ring3: window now ";
    while (*a) b[l++] = *a++;
    for (int k = 0; k < 2; k++) {
        unsigned v = k ? n.height : n.width, d = 1000000000u; int on = 0;
        for (; d; d /= 10) { unsigned q = v / d % 10; if (q || on || d == 1) { b[l++] = (char)('0' + q); on = 1; } }
        if (!k) b[l++] = 'x';
    }
    b[l++] = '\n';
    jt_write(1, b, (unsigned)l);
    return 1;
}

#endif
