/* M3c: the app table. Each user program the ARM kernel can load is the unchanged user/NAME.c built for aarch64 (see the
   Makefile: -fPIE, linked by user_c.ld into uapp_NAME.bin) and baked in here with .incbin. user_apps is what main.c's
   user_load walks: { name, start, end } until a zero name. The image carries its own page split and relocation list in a
   16 byte trailer (user_c.ld), so the table needs nothing else. A new app is one `app` line and one table row. A row
   whose name matches a dock tile (case aside) is what that tile opens: calendar, and since M3d notes. */
.macro app name
    .balign 16
    .global \name\()_img, \name\()_img_end
\name\()_img:
    .incbin "uapp_\name\().bin"
\name\()_img_end:
.endm

    .section .rodata.userapps, "a"
    app hello
    app calendar
    app notes

/* M3c part 2: loop, the scheduler's test program, made by hand in the same image format: one code page, then the trailer
   { "JTU1", 4096 bytes of code, an empty relocation list }. It asks for its window, then yields forever: it never polls,
   never draws and never exits, so Esc does nothing and only its red dot ends it. The desktop must stay usable while it runs
   (tools/checks/arm64-calendar-check.py). */
    .balign 16
loop_img:
    sub sp, sp, #32
    mov x0, sp                  /* a struct jt_window_info on its stack */
    mov x8, #384                /* JT_SYS_WINDOW_OPEN */
    svc #0
1:  mov x8, #158                /* JT_SYS_SCHED_YIELD */
    svc #0
    b 1b
    .skip 4096 - (. - loop_img)
    .long 0x3155544a, 4096, 4096, 4096
loop_img_end:

.Lname_hello:    .asciz "hello"
.Lname_calendar: .asciz "calendar"
.Lname_notes:    .asciz "notes"
.Lname_loop:     .asciz "loop"

    .balign 8
    .global user_apps
user_apps:
    .quad .Lname_hello, hello_img, hello_img_end
    .quad .Lname_calendar, calendar_img, calendar_img_end
    .quad .Lname_notes, notes_img, notes_img_end
    .quad .Lname_loop, loop_img, loop_img_end
    .quad 0, 0, 0
