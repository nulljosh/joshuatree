/* M3c: the app table. Each user program the ARM kernel can load is the unchanged user/NAME.c built for aarch64 (see the
   Makefile: -fPIE, linked by user_c.ld into uapp_NAME.bin) and baked in here with .incbin. user_apps is what main.c's
   user_load walks: { name, start, end } until a zero name. The image carries its own page split and relocation list in a
   16 byte trailer (user_c.ld), so the table needs nothing else. A new app is one `app` line and one table row. A row
   whose name matches a dock tile (case aside) is what that tile opens: calendar, and since M3d notes. music (M3e) has no
   tile, a decision: Spotlight lists it (main.c SPOT_MUSIC). books (2.66) is the same: Spotlight only (SPOT_BOOKS), and finder (3.14.0): Spotlight and Samantha's open (SPOT_FINDER). mail (3.15.0) is the Mail tile's, built from user/mailpi.c. */
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
    app music
    app books
    app finder
    app mail

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

/* M3e: fpkeep, the EL0 floating point test program, made by hand like loop (tools/checks/arm64-music-check.py runs it with
   `userrun fpkeep`). It fills q0-q31 with known patterns and sets FPCR and FPSR, then 40 times yields (the app is parked
   and resumed) and asks its pid (a call that returns at once), and compares every one of them: exit 0 when they all came
   back, 1 when one did not. The musictest kernel wipes the FPU inside every call and before every resume (fptest.c's
   fp_clobber), so only vectors.S keeping an app's FP registers lets it exit 0. */
    .arch_extension fp
    .arch_extension simd
    .balign 16
fpkeep_img:
    movz x9, #0xcdef            /* x9: the pattern, 0x0123456789abcdef plus 0x1111111111111111 per register (fptest.c's) */
    movk x9, #0x89ab, lsl #16
    movk x9, #0x4567, lsl #32
    movk x9, #0x0123, lsl #48
    mov x15, x9
    mov x10, #0x1111111111111111
    .irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    dup v\n\().2d, x9
    add x9, x9, x10
    .endr
    movz x11, #0x40, lsl #16    /* FPCR: round toward plus infinity, not the reset value */
    msr fpcr, x11
    movz x12, #0x000f           /* FPSR: QC and four sticky exception flags */
    movk x12, #0x0800, lsl #16
    msr fpsr, x12
    mov x19, #40
1:  mov x8, #158                /* JT_SYS_SCHED_YIELD: parked, the desktop runs, resumed */
    svc #0
    mov x8, #20                 /* JT_SYS_GETPID */
    svc #0
    subs x19, x19, #1
    b.ne 1b
    mov x9, x15
    mov x13, xzr
    .irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    mov x14, v\n\().d[0]
    eor x14, x14, x9
    orr x13, x13, x14
    mov x14, v\n\().d[1]
    eor x14, x14, x9
    orr x13, x13, x14
    add x9, x9, x10
    .endr
    mrs x14, fpcr
    eor x14, x14, x11
    orr x13, x13, x14
    mrs x14, fpsr
    eor x14, x14, x12
    orr x13, x13, x14
    cmp x13, #0
    cset x0, ne
    mov x8, #1                  /* JT_SYS_EXIT */
    svc #0
2:  b 2b
    .skip 4096 - (. - fpkeep_img)
    .long 0x3155544a, 4096, 4096, 4096
fpkeep_img_end:

.Lname_hello:    .asciz "hello"
.Lname_calendar: .asciz "calendar"
.Lname_notes:    .asciz "notes"
.Lname_music:    .asciz "music"
.Lname_books:    .asciz "books"
.Lname_finder:   .asciz "finder"
.Lname_mail:     .asciz "mail"
.Lname_loop:     .asciz "loop"
.Lname_fpkeep:   .asciz "fpkeep"

    .balign 8
    .global user_apps
user_apps:
    .quad .Lname_hello, hello_img, hello_img_end
    .quad .Lname_calendar, calendar_img, calendar_img_end
    .quad .Lname_notes, notes_img, notes_img_end
    .quad .Lname_music, music_img, music_img_end
    .quad .Lname_books, books_img, books_img_end
    .quad .Lname_finder, finder_img, finder_img_end
    .quad .Lname_mail, mail_img, mail_img_end
    .quad .Lname_loop, loop_img, loop_img_end
    .quad .Lname_fpkeep, fpkeep_img, fpkeep_img_end
    .quad 0, 0, 0
