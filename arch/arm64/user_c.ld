/* Link script for a flat ARM64 EL0 program (M3b, grown in M3c): the unchanged user/*.c, built -fPIE and linked -pie at 0, then
   written out headerless. The kernel (main.c, user_load) copies the image to the app window, applies the R_AARCH64_RELATIVE
   entries listed in .rela.dyn (a month-name table is a pointer table, so a data pointer needs the base added) and enters at
   the first byte, so _start comes first. The last 16 bytes are a trailer the loader reads, because the image table only
   knows a name, a start and an end:
       +0 "JTU1"   +4 bytes of code and read-only data (a page multiple: those pages are mapped read-only and executable,
                   everything after them read-write and never executable)   +8 .rela.dyn start   +12 .rela.dyn end
   Memory past _user_end is zeroed by the loader (a program such as user/calendar.c keeps its event list there), so a
   .bss is fine too, as long as it fits the pool the loader maps. */
ENTRY(_start)
SECTIONS {
    . = 0;
    .text : { *(.text.start) *(.text*) }
    .rodata : ALIGN(16) { *(.rodata*) }
    . = ALIGN(4096);
    __jt_code_end = .;
    .data.rel.ro : { *(.data.rel.ro*) }
    .data : ALIGN(16) { *(.data*) }
    .got : ALIGN(8) { *(.got*) }
    .rela.dyn : ALIGN(8) { __jt_rela_start = .; *(.rela.dyn) __jt_rela_end = .; }
    .jt_trailer : ALIGN(4) { LONG(0x3155544a) LONG(__jt_code_end) LONG(__jt_rela_start) LONG(__jt_rela_end) }
    _user_end = .;
    .bss : { *(COMMON) *(.bss*) }
    _user_bss_end = .;
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.dynamic) *(.dynsym) *(.dynstr) *(.hash) *(.gnu.hash) *(.interp) }
}
ASSERT(SIZEOF(.got) == 0, "a GOT means the code was not built -fPIE -fdirect-access-external-data")
ASSERT(_user_end <= 0x40000, "the user image must fit the 256 KiB app window (UP_APP..UP_ASTACK in main.c)")
ASSERT(_user_bss_end - _user_end <= 0x8000, "the zeroed pool past the image is 32 KiB (APP_ZERO_PAGES in main.c)")
