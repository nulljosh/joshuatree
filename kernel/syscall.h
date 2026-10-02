#ifndef SYSCALL_H
#define SYSCALL_H
/* The int 0x80 syscall ABI. x86 Linux convention, borrowed rather than
   invented: eax = syscall number, ebx/ecx/edx = arguments 1-3, result
   returned in eax, negative errno on failure. The numbers below are Linux
   i386's own (__NR_exit = 1, __NR_write = 4), so anything written against
   the real ABI's calling convention lines up here too.
   docs/SYSCALL-ABI.md is the contract; this header and syscall.c are its
   implementation, and user/jtsys.h is the only thing a ring-3 program
   needs to speak it.
   v64 (0.61.0) shipped exit and write. The rest of the v1 set (read, open,
   close, time, getpid, sched_yield) lands here alongside user/hello.c, the
   first real external caller of any of it.

   v2 adds file writing (open flags, write to a descriptor from open,
   write-back on close), lseek, and argv. It adds numbers and flag bits
   and never changes what a v1 number means: v1 is frozen, and
   tools/checks/usertest-check.sh still runs user/hello.c unmodified. */

#define SYS_EXIT         1   /* ebx = exit code; ends the calling task, never returns */
#define SYS_READ         3   /* ebx = fd, ecx = buf, edx = len; bytes read, 0 at end of file */
#define SYS_WRITE        4   /* ebx = fd (1 or 2 only), ecx = buf, edx = len; returns bytes written */
#define SYS_OPEN         5   /* ebx = path, ecx = flags (0 only); returns a per-task fd >= 3 */
#define SYS_CLOSE        6   /* ebx = fd; 0, or -EBADF */
#define SYS_TIME        13   /* ebx = 0, or a user unsigned* to store into; returns seconds since the epoch */
#define SYS_LSEEK       19   /* v2: ebx = fd, ecx = offset, edx = whence; returns the new position */
#define SYS_GETPID      20   /* returns the calling task's slot id */
#define SYS_SCHED_YIELD 158  /* gives up the rest of this quantum; returns 0 */

/* v3 (1.7.7): windows. Joshua Tree's own numbers start at 384, past
   anything Linux i386 assigns that this kernel could ever want to borrow,
   so the two sets can never collide. Same register shape as everything
   above: ebx/ecx/edx, result in eax, negative errno on failure. */
#define SYS_WINDOW_OPEN 384  /* ebx = struct jt_window_info* (user); fills it, maps the framebuffer user-accessible; 0 or -errno */
#define SYS_WINDOW_POLL 385  /* ebx = struct jt_event* (user), ecx = flags (JT_POLL_PRESENT); 1 event written, -EAGAIN none */

#define SYS_TASKS       386  /* 1.9.6: ebx = struct jt_tasks* (user), ecx = slot to kill or -1; fills the snapshot, 0 or -errno */

/* 1.9.11: SYS_HTTP_GET, the one call the Curbfind port needed. One HTTP
   GET to the fixed host joshuatree.heyitsmejosh.com, port 80: userland
   names only the path, never the host, so a ring-3 program cannot point
   the kernel's network stack anywhere else.
     ebx = path (user, NUL-terminated): at most JT_HTTP_PATH_MAX bytes
           before the NUL, must start with '/', printable ASCII 0x21..0x7E
           only (no spaces, no CR or LF, nothing that could end the request
           line or start a header); anything else is -EINVAL, and -EFAULT
           if the string walks off user memory.
     ecx = out buffer (user, writable), edx = its length, clamped to
           JT_HTTP_BIG_MAX (64KB); -EFAULT unless the whole range is user memory.
           Up to JT_HTTP_BODY_MAX (2KB) the reply is bounced and copied out only
           on a 200. Above it the reply is received straight into this buffer
           (3s budget), so a failed fetch may leave scratch bytes in it; trust
           only the returned count.
   Returns the body byte count (0..edx) when the reply was HTTP 200.
   A non-200 reply returns minus its status, -(100..599), which can never
   collide with an errno (all below 100). -ENODEV when there is no NIC,
   -EIO when the host did not answer within the 1500ms reply budget.
   Nothing is written to the buffer on any failure. */
#define SYS_HTTP_GET    387
#define JT_HTTP_PATH_MAX 128
#define JT_HTTP_BODY_MAX 2048
#define JT_HTTP_BIG_MAX  65536
/* 1.9.13: SYS_READDIR, the directory listing Search needs and Files will
   reuse. 387 is SYS_HTTP_GET (Curbfind, 1.9.11), so this is 388.
     ebx = path (user, NUL-terminated, at most PATH_MAX = 63 bytes before
           the NUL; longer, or no NUL within 64 bytes, is -EINVAL)
     ecx = struct jt_dirent* (user), an array of edx records
     edx = how many records the array holds; clamped to JT_READDIR_MAX
   Returns the number of entries in the directory, which can be larger
   than edx: only the first edx are written, so a caller that gets back
   more than it asked for knows the listing was cut. Or -errno: -EFAULT
   (a pointer that is not user memory for the whole array), -EINVAL (path
   too long, an empty, "." or ".." component, a leading slash, more than
   JT_PATH_DEPTH components), -ENOENT (a component is not a directory, or
   the backend has no directories at all, as ramfs does not).
   The path is relative to the shell's current directory and walked one
   component at a time with vfs_chdir, then walked back with ".." before
   the call returns, all inside the int 0x80 gate with interrupts off, so
   ring 3 never moves the kernel's own cwd: a program that wants to
   descend keeps its own cwd string and passes it here. "" and "." list
   the current directory. SYS_OPEN takes the same relative paths since
   1.9.13, so a file a listing named under DOCS opens as "DOCS/NAME".
   Every record is fixed-size: the name NUL-terminated and cut to fit,
   the size in bytes (0 for a directory), and is_dir 1 or 0. The array is
   only written after the whole range passed paging_user_range_ok, and
   never past edx records. */
#define SYS_READDIR     388
#define JT_DIRENT_NAME  32   /* ramfs's own name cap; FAT 8.3 names are 12 */
#define JT_READDIR_MAX  64   /* records per call, the most any backend lists today (files.h's own cap) */
#define JT_PATH_DEPTH    8   /* components a relative path may have */
struct jt_dirent { char name[JT_DIRENT_NAME]; unsigned int size, is_dir; };

#define JT_POLL_PRESENT 1    /* copy the framebuffer to the screen before looking for an event */

/* Event kinds SYS_WINDOW_POLL writes. a/b depend on the kind: KEY carries
   the key in a (ASCII, or app.h's KEY_* codes at 256 and up), CLICK the
   pointer in window coordinates (a = x, b = y), WHEEL the direction in a
   (+1 up, -1 down). */
#define JT_EV_KEY   1
#define JT_EV_CLICK 2
#define JT_EV_WHEEL 3

struct jt_window_info { unsigned int width, height, pitch; unsigned int *pixels; };
struct jt_event { unsigned int kind; int a, b; };
/* SYS_TASKS: uptime ticks (100Hz), free and total memory in KB, the calling
   task's own slot, and a bitmask of the used scheduler slots. */
struct jt_tasks { unsigned int ticks, free_kb, total_kb, current, used; };

/* v2 open() flags. Linux i386's own values, the same borrow the call
   numbers are: O_RDONLY/O_WRONLY/O_RDWR are the low two bits, the rest
   are independent bits. v1 froze "flags must be 0", which is exactly
   O_RDONLY with no modifiers, so every v1 open still means what it meant.
   Only these bits are understood; any other bit set is -EINVAL rather
   than silently ignored, so a program cannot think it asked for
   something this kernel did not do. */
#define JT_O_RDONLY  0x0000
#define JT_O_WRONLY  0x0001
#define JT_O_RDWR    0x0002
#define JT_O_ACCMODE 0x0003
#define JT_O_CREAT   0x0040
#define JT_O_TRUNC   0x0200
#define JT_O_APPEND  0x0400

/* lseek whence values, Linux's own. */
#define JT_SEEK_SET 0
#define JT_SEEK_CUR 1
#define JT_SEEK_END 2

/* Sized by the highest number in the v1 set (158, sched_yield) rounded up
   to the next multiple of 32, which is the only real reason to pick 160
   over 159: it keeps the table a whole number of cache lines and leaves
   room for the next few Linux numbers worth adopting without another
   resize. It is a table of function pointers, 640 bytes of .bss, so
   size here is not a cost worth optimising. Every slot that is not
   assigned below is a null the dispatcher turns into -ENOSYS rather than
   a jump into nothing, and any number >= NSYSCALLS gets the same answer,
   so the gaps in Linux's numbering cost nothing and hide nothing. */
/* SYS_MKDIR and SYS_UNLINK, what the Notes app makes folders and deletes
   notes with. Both take one path, ebx, resolved exactly like SYS_OPEN: relative,
   split in place, every part but the last entered with vfs_chdir and walked
   back before the call returns; the last part is the name made or removed.
     mkdir  -> 0, or -errno: -EFAULT, -EINVAL (malformed path), -ENOENT (a
               parent is not a folder, or the backend has no folders, ramfs),
               -ENOSPC (name taken, disk full or directory full: the backend
               says only yes or no).
     unlink -> 0, or -errno: -EFAULT, -EINVAL, -ENOENT (no such file),
               -EISDIR (the name is a folder; unlink never removes one).
   Neither is reachable from a path that open could not reach. */
#define SYS_MKDIR       389
#define SYS_UNLINK      390
/* 1.9.24: SYS_SHELL_RUN, the window Terminal's shell. ebx = const char *line (user, NUL terminated,
   at most JT_SHELL_LINE_MAX bytes: "<cwd>\n<command>", cwd a relative path or empty for the root), ecx = char *out (user), edx = outlen. Returns bytes written to out
   (NUL terminated, truncated to outlen-1), or -EFAULT (either pointer), -EINVAL (outlen 0 or over 4096,
   line too long). A refused command still returns a one-line message. Allowlist in kernel/shellsys.c. */
#define SYS_SHELL_RUN   391
#define JT_SHELL_LINE_MAX 160 /* same number as user/jtsys.h: cwd (63) + newline + command (95) */
/* 1.9.26: SYS_HTTP_POST, the first slice of Samantha at ring 3 (docs/ARCHITECTURE.md,
   "Samantha at ring 3"). One HTTP POST of application/json to the chat host the
   Settings app keeps (llm_host/llm_port in kernel.c, turing.heyitsmejosh.com:80 by
   default): the program names only the path, never the host, the same rule as
   SYS_HTTP_GET. The six arguments do not fit three registers, so ebx is one struct:
     ebx = struct jt_http_post* (user, read): path (user, NUL terminated, same
           rules as http_get: at most JT_HTTP_PATH_MAX, starts with '/', printable
           ASCII 0x21..0x7E), body + body_len (user, read, at most
           JT_HTTP_POST_BODY_MAX), out + out_len (user, write, clamped to
           JT_HTTP_POST_REPLY_MAX), reply_ticks (0 = JT_HTTP_POST_TICKS_DEFAULT,
           clamped to JT_HTTP_POST_TICKS_MAX).
     ecx, edx unused (0).
   Returns the reply body byte count (0..out_len) on HTTP 200; a non-200 reply
   returns minus its status, -(100..599); -ENODEV no NIC, -EIO no answer in time,
   -EBUSY a fetch already in flight, -EFAULT any range outside user memory,
   -EINVAL a bad path or an over-long body. Nothing is written to out on failure.
   The body is copied into a kernel bounce buffer before the network is touched,
   so a program cannot change it mid-request.
   ecx = flags (0 = the original behaviour, so old callers are unchanged):
     JT_POST_WORKER: send to the fixed joshuatree Worker host (the one SYS_HTTP_GET
       uses), port 80, instead of the Settings chat host. A selector, never a string.
     JT_POST_BIG: body up to JT_HTTP_BIG_MAX read straight from the caller buffer and
       reply up to JT_HTTP_BIG_MAX written straight into out (no kernel bounce; the net
       layer copies the body into its own request buffer). On failure out may hold
       scratch bytes. Unknown flag bits are -EINVAL. */
#define JT_POST_WORKER 1u
#define JT_POST_BIG    2u
#define SYS_HTTP_POST   392
#define JT_HTTP_POST_BODY_MAX  6144 /* chat.h's req_body cap: full history to /api/chat */
#define JT_HTTP_POST_REPLY_MAX 8192 /* chat.h's resp cap for /api/chat */
#define JT_HTTP_POST_TICKS_DEFAULT 1500 /* 15s at 100Hz */
#define JT_HTTP_POST_TICKS_MAX     4500 /* 45s, chat.h's CHAT_SEND_TIMEOUT_TICKS */
struct jt_http_post {
    const char *path;
    const char *body; unsigned int body_len;
    char *out;        unsigned int out_len;
    unsigned int reply_ticks;
};
/* kernel.c: the Settings-owned chat host, read by SYS_HTTP_POST. */
const char *llm_host_get(void);
int llm_port_get(void);
/* 1.9.26: SYS_SYSINFO, the read-only state Samantha reports (weather, host, phone flag, clock).
   ebx = struct jt_sysinfo* (user, write), ecx = the caller's sizeof (so the struct can grow:
   an old program passes a smaller size and gets only the fields it knows, a new one on an old
   kernel sees a short count). ecx must be at least 8 (version + size). The kernel fills a
   private copy and copies out min(ecx, sizeof) bytes; returns that count, or -EFAULT (range
   outside user memory), -EINVAL (ecx under 8). version is JT_SYSINFO_VERSION and comes first. */
#define SYS_SYSINFO     395
#define JT_SYSINFO_VERSION 1
#define JT_WX_TEXT_MAX  24
#define JT_SYSINFO_HOST_MAX 40
struct jt_sysinfo {
    unsigned int version;       /* JT_SYSINFO_VERSION; always first */
    unsigned int size;          /* sizeof this struct in the kernel that filled it */
    unsigned int phone;         /* 1 when booted in phone mode */
    unsigned int epoch;         /* seconds since 1970, same as SYS_TIME */
    unsigned int wx_have;       /* 1 when a good weather reading exists */
    unsigned int wx_state;      /* 0 never tried, 1 ok, 2 offline, 3 timeout, 4 failed, 5 bad */
    int wx_temp_c;              /* whole degrees C, valid when wx_have */
    int wx_code10;              /* WMO code times 10, valid when wx_have */
    unsigned int llm_port;      /* Settings-owned chat host port */
    char wx_text[JT_WX_TEXT_MAX];          /* the menu bar text, NUL terminated, empty when none */
    char llm_host[JT_SYSINFO_HOST_MAX];    /* the chat host SYS_HTTP_POST talks to */
};
/* 1.9.26: SYS_LAUNCH_REQUEST, "open <app>" for a ring-3 Samantha. ebx = const char *name (user,
   NUL terminated, at most JT_APP_NAME_MAX bytes). The name must match an APPS[] row exactly
   (case sensitive, real apps only, not the Apps folder or Trash). The kernel records ONE pending
   index and returns at once; it never launches inside the gate. The desktop loop takes the
   index on its next pass and opens it the way a dock click does. Returns 0, -EFAULT, -EINVAL
   (no such app, name too long, empty), or -EBUSY (a request is already pending). */
#define SYS_LAUNCH_REQUEST 396

/* 1.9.27: SYS_BRK (397), per-task heap. ebx = new top (0 queries). Returns the heap top after
   the call: JT_BRK_BASE on a fresh task, or -EINVAL for a top outside [JT_BRK_BASE,
   JT_BRK_BASE + JT_BRK_MAX_PAGES * 4096] and -ENOMEM when the PMM cannot back the growth
   (the old top stands). Pages are zeroed, user+writable, mapped into this task's directory
   only, and freed on exit or crash (kernel/brk.c). */
#define SYS_BRK         397
/* 1.9.28: SYS_CLIPBOARD (399), the one system clipboard. ebx = op, ecx = user buffer, edx = length.
   JT_CLIP_SET (1) replaces the clipboard with len bytes (0 clears; at most JT_CLIP_MAX, else -EINVAL)
   and returns len. JT_CLIP_GET (2) copies out at most len bytes and returns how many it copied (0
   when empty); a len shorter than the clipboard is a clipped paste, not an error. -EFAULT for a bad
   pointer, -EINVAL for an unknown op. The kernel logs `CLIPCOPY:<n>` and `CLIPPASTE:<n>` (plus
   `:<fnv1a32>` under the `cliptrace` boot flag, and `CLIPTRUNC` after a clipped paste) but never
   the text itself. */
#define SYS_CLIPBOARD   399
#define JT_CLIP_SET     1
#define JT_CLIP_GET     2
#define JT_CLIP_MAX     4096
#define JT_APP_NAME_MAX 24
/* 1.9.26: SYS_AUDIO (393), audio out for a ring-3 Samantha. One number, three ops. ebx = op,
   ecx = const/non-const struct pointer (user), edx = the caller's sizeof that struct.
   Format is what sb16_play and /api/speak already use: 8-bit UNSIGNED mono PCM, 4000..44100 Hz
   (the Worker sends 16000). The kernel copies into a 32KB ring and the SB16 IRQ drains it in 4KB
   DMA transfers; the gate never waits.
   JT_AUDIO_PLAY (1): struct jt_audio_play {pcm, len, rate, flags}. Copies at most
     JT_AUDIO_CHUNK_MAX (8192) bytes per call, fewer if the ring is full. Returns the bytes taken
     (0 means full or busy: retry on a later frame). Playback starts when 4KB are queued, or at
     once if flags has JT_AUDIO_END (set it on the call that queues the last bytes of a clip).
     rate is read only when the queue was idle. -EFAULT bad range, -EINVAL len 0 / bad op /
     short struct, -ENODEV no card.
   JT_AUDIO_STATUS (2): fills struct jt_audio_status {version,size,playing,queued,space,rate,
     played}, copies out min(edx, sizeof) bytes, returns that count. played is in samples
     (bytes) since the queue last went idle, so mouth time = played * 1000 / rate ms.
   JT_AUDIO_STOP (3): drops what is not yet in flight (about a quarter second still finishes).
     Returns 0. */
#define SYS_AUDIO       393
#define JT_AUDIO_PLAY   1
#define JT_AUDIO_STATUS 2
#define JT_AUDIO_STOP   3
#define JT_AUDIO_END    1
/* 1.9.26: SYS_AUDIO_RECORD (394), push-to-talk capture. Same format (8-bit unsigned mono) and SB16
   card as 393, so it is exclusive with playback. The IRQ fills a 32KB kernel ring from 4KB ADC
   transfers; nothing here waits. ebx = op.
   JT_REC_START (1): ecx = rate in Hz (a value, not a pointer; clamped to 4000..44100), edx unused.
     Clears the ring and arms capture (a take left on is restarted). Returns 0, -ENODEV no card,
     -EBUSY playback queued/running, or a transfer from a previous take still landing (retry).
   JT_REC_READ (2): ecx = user buffer, edx = max bytes (capped at JT_REC_CHUNK_MAX, 8192).
     Copies what is banked so far, oldest first, and returns the count (0 = nothing yet). Still
     drains the remainder after STOP. -EFAULT bad range, -EINVAL edx 0. If the caller falls more
     than 32KB behind, the oldest samples are dropped.
   JT_REC_STOP (3): ends capture; the 4KB in flight (about a quarter second at 16 kHz) still lands
     and stays readable. Returns 0. */
#define SYS_AUDIO_RECORD 394
#define JT_REC_START    1
#define JT_REC_READ     2
#define JT_REC_STOP     3
#define JT_REC_CHUNK_MAX 8192
#define JT_AUDIO_CHUNK_MAX 8192
#define JT_AUDIO_STATUS_VERSION 1
struct jt_audio_play { const void *pcm; unsigned int len; unsigned int rate; unsigned int flags; };
struct jt_audio_status {
    unsigned int version;       /* JT_AUDIO_STATUS_VERSION; always first */
    unsigned int size;          /* sizeof this struct in the kernel that filled it */
    unsigned int playing;       /* 1 while a transfer is in flight or bytes are queued */
    unsigned int queued;        /* bytes waiting in the ring */
    unsigned int space;         /* bytes the ring can still take */
    unsigned int rate;          /* Hz of the clip in the ring */
    unsigned int played;        /* samples heard so far in this clip */
};
/* kernel.c: SYS_SYSINFO fill, SYS_LAUNCH_REQUEST validate and store, desktop loop take. */
void jt_sysinfo_fill(struct jt_sysinfo *si);
int jt_launch_request(const char *name);
int jt_launch_take(void);
void jt_facehost_cmdline(const char *cl); /* syscall.c: facehost=HOST[:PORT] */
void jt_clip_cmdline(const char *cl);     /* syscall.c: cliptrace adds a content hash to the CLIPCOPY/CLIPPASTE lines */
#define NSYSCALLS 416 /* 385 (SYS_WINDOW_POLL) rounded up to a multiple of 32; was 160 before v3. 386 (tasks), 387 (http_get), 388 (readdir) and 392 (http_post) fit under it */

/* Exactly the stack shape syscall_entry (isr.S) builds, lowest address
   first: the four data segments pushed last, pusha's eight, then the CPU's
   own privilege-change frame. useresp/ss only exist when the caller was
   ring 3; nothing here reads them. */
struct syscall_frame {
    unsigned int gs, fs, es, ds;
    unsigned int edi, esi, ebp, esp_at_pusha, ebx, edx, ecx, eax;
    unsigned int eip, cs, eflags;
    unsigned int useresp, ss;
};

void syscall_install(void); /* wires vector 0x80 into the IDT with DPL 3 */
void syscall_dispatch(struct syscall_frame *f); /* called from syscall_entry; writes the result into f->eax */

/* Closes every descriptor a task still had open, freeing their buffers.
   Called by task_exit_with() on the way out, so a program that exits
   without closing leaks nothing; a task slot is reused by the next
   task_create, and a stale fd table would hand the new task the previous
   one's open files. */
void syscall_release_task(int id);

/* 1.7.7: slot id of the task that owns the ring-3 window, or -1. The
   launcher (kernel/ring3app.c) reads it after exec_user returns to say
   whether the window was released cleanly. */
int syscall_window_owner(void);
#endif

/* 1.9.23: compositor windows for ring-3 programs (kernel/syscall.c r3wins). */
void syscall_windows_init(void);
int  syscall_window_register(int task, unsigned int w, unsigned int h, void *image);
const unsigned int *syscall_window_fb(int task, unsigned int *w, unsigned int *h, int *dirty); /* kernel pointer to the window's buffer, clears dirty */
void syscall_window_push_event(int task, int kind, int a, int b);
