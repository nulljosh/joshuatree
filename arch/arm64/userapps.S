/* M3c: the app table. Each user program the ARM kernel can load is the unchanged user/NAME.c built for aarch64 (see the
   Makefile: -fPIE, linked by user_c.ld into uapp_NAME.bin) and baked in here with .incbin. user_apps is what main.c's
   user_load walks: { name, start, end } until a zero name. The image carries its own page split and relocation list in a
   16 byte trailer (user_c.ld), so the table needs nothing else. A new app is one `app` line and one table row. */
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

.Lname_hello:    .asciz "hello"
.Lname_calendar: .asciz "calendar"

    .balign 8
    .global user_apps
user_apps:
    .quad .Lname_hello, hello_img, hello_img_end
    .quad .Lname_calendar, calendar_img, calendar_img_end
    .quad 0, 0, 0
