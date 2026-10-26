/* kmain, the i386 kernel's entry, out of kernel.c since 4.25.1: multiboot and memory, paging, the heap,
   interrupts, disks and the filesystems, the network, the boot line's flags, then the desktop (gui_run) or the
   text shell. boot/boot.S calls it. */
#include "gdt.h"
#include "idt.h"
#include "irq.h"
#include "pic.h"
#include "pmm.h"
#include "paging.h"
#include "kheap.h"
#include "picker.h"
#include "task.h"
#include "ring3.h"
#include "syscall.h"
#include "ata.h"
#include "fat.h"
#include "vfs.h"
#include "blockdev.h"
#include "ramdisk.h"
#include "trash.h"
#include "ramfs.h"
#include "exec.h"
#include "libc.h"
#include "pci.h"
#include "vbe.h"
#include "mouse.h"
#include "vmmouse.h"
#include "window.h"
#include "ttf.h"
#include "font.h"
#include "rtl8139.h"
#include "net.h"
#include "gui_prims.h"
#include "dock_geom.h"
#include "dock_draw.h"
#include "gui_paint.h"
#include "http.h"
#include "wallmap.h"
#include "gui_shapes.h"
#include "shell.h"
#include "selftest.h"
#include "desktop.h"
#include "multiwin.h"
#include "walltint.h"
#include "settings.h"
#include "serial.h"
#include "sb16.h"
#include "speak.h"
#include "app.h"
#include "png.h"
#include "jpeg.h"
#include "version.h"
#include "ring3app.h"
#include "weather.h"
#include "json.h"
#include "html.h"
#include "klog.h"
#include "hint.h"
#include "phone_home.h"
#include "wallpaint.h"
#include "menubar.h"
#include "icons.h"
#include "clockicon.h"
#include "appview.h"
#include "auth.h"
#include "uitext.h"
#include "apps_geom.h"
#include "appsfolder.h"
#include "settings_ui.h"
#include "stocks.h"
#include "bench.h"
#include "bootscreen.h"
#include "menu.h"
#include "console.h"
#include "symtab.h"
#include "entropy.h"
#include "user_wc.h"   /* the wc program kmain installs; only this file includes its bytes */

static int bench_at_boot = 0;
static int panic_test_at_boot = 0;
void kmain(unsigned int multiboot_info_addr){
    serial_init();
    serial_puts("=== kmain boot start === v" JT_VERSION_STR "\n"); entropy_init(); /* 1.7.10: seed the DRBG before anything asks for a salt */
    /* 1.0.12: llmhost=/llmport= command-line overrides for llm_host/llm_port
       (declared way below), parsed alongside wxhost= but applied AFTER
       settings_load() runs (see its call site) so a stale/persisted
       SETTINGS.TXT can never silently win over an override the boot
       command line explicitly asked for -- the same problem wxhost=
       never has to solve since it feeds a variable settings_load() never
       touches. Empty/zero means "no override," the ordinary compiled-in
       default or whatever Settings has saved stands. */
    char llm_host_override[LLM_HOST_MAX] = "";
    int llm_port_override = 0;
    /* 2.14.0: claudehost=/claudeport=/claudetoken=, the same one-boot override for the Claude
       relay (tools/checks/ring3claude-check.py boots with no disk). Applied after settings_load,
       never saved. The token is never echoed to serial. */
    char claude_host_override[CLAUDE_HOST_MAX] = "";
    char claude_token_override[CLAUDE_TOKEN_MAX] = "";
    int claude_port_override = 0;
    /* Multiboot command line (flags bit 2, pointer at +16), read here while
       the bootloader's low memory is still identity-reachable. Only one
       option exists: wxhost=A.B.C.D[:PORT], see wx_override_host. */
    /* Multiboot info flag bit 12: the bootloader set a framebuffer. Offsets per the
       multiboot1 spec: addr 88 (u64, low half used), pitch 96, width 100, height 104,
       bpp 108, type 109 (1 = direct RGB). Read only while the info block sits inside
       the boot identity map. */
    if (multiboot_info_addr && multiboot_info_addr < 0x400000 && (*(unsigned int *)multiboot_info_addr & (1u << 12))) {
        const unsigned char *mb = (const unsigned char *)multiboot_info_addr;
        if (mb[109] == 1 && *(const unsigned int *)(mb + 92) == 0)
            vbe_set_boot_framebuffer(*(const unsigned int *)(mb + 88), *(const unsigned int *)(mb + 96),
                                     *(const unsigned int *)(mb + 100), *(const unsigned int *)(mb + 104), mb[108]);
    }
    if (multiboot_info_addr && (*(unsigned int *)multiboot_info_addr & 0x4)) {
        const char *cl = (const char *)*(unsigned int *)(multiboot_info_addr + 16);
        const char *cl0 = cl; /* the loop below mutates cl directly; llmhost=/llmport= (further down) need the untouched start */
        for (const char *pc = cl; pc && *pc; pc++)
            if (pc[0]=='p' && pc[1]=='o' && pc[2]=='r' && pc[3]=='t' && pc[4]=='f' && pc[5]=='o' && pc[6]=='l' && pc[7]=='i' && pc[8]=='o') { portfolio_dock = 1; serial_puts("portfolio dock\n"); break; }
        for (const char *pc = cl; pc && *pc; pc++)
        for (const char *pc = cl; pc && *pc; pc++)
            if (pc[0]=='b' && pc[1]=='e' && pc[2]=='n' && pc[3]=='c' && pc[4]=='h' && (pc[5]==' ' || pc[5]==0)) { bench_at_boot = 1; break; }
        for (const char *pc = cl; pc && *pc; pc++)
            if (pc[0]=='s' && pc[1]=='a' && pc[2]=='m' && pc[3]=='a' && pc[4]=='n' && pc[5]=='t' && pc[6]=='h' && pc[7]=='a') { boot_to_samantha = 1; serial_puts("bootsamantha\n"); break; }
        /* "phone" on the command line: Bochs VBE takes any mode, so this
           just swaps gui_run's video mode for a real portrait phone size
           (430x760) instead of the desktop's 960x540@2x. Landing's embed.js
           always pairs this with samantha (a phone visitor gets her full-
           screen view, not the icon desktop laid out for a mouse), but
           phone alone still forces it here so booting with just "phone"
           never lands on a desktop that was never designed for 430px. */
        for (const char *pc = cl; pc && *pc; pc++)
            if (pc[0]=='p' && pc[1]=='h' && pc[2]=='o' && pc[3]=='n' && pc[4]=='e' && (pc[5]==' ' || pc[5]==0)) { boot_to_phone = 1; boot_to_samantha = 1; serial_puts("bootphone\n"); break; }
        if (gui_parse_res(cl, &boot_res_w, &boot_res_h)) serial_puts("bootres\n"); /* dock_geom.c: "res=WxH" */
        /* "panictest" on the multiboot command line (tools/checks/
           panic-symbols-check.py passes it) -- deliberately faults from a
           known, named function right after idt_install() so a headless
           check can prove the crash report's symbol table names it. Never
           set on a normal boot, so this path can never fire outside the
           check that asks for it. */
        for (const char *pc = cl; pc && *pc; pc++)
            if (pc[0]=='p' && pc[1]=='a' && pc[2]=='n' && pc[3]=='i' && pc[4]=='c' && pc[5]=='t' && pc[6]=='e' && pc[7]=='s' && pc[8]=='t') { panic_test_at_boot = 1; break; }
        /* nodhcp: skip the DHCP DISCOVER/OFFER/REQUEST/ACK exchange
           net_init now runs by default (drivers/net.c) and boot straight
           onto the hardcoded 10.0.2.15/SLIRP-gateway config every net_init
           caller already passes. Parsed before anything else in kmain
           could call net_init. */
        for (const char *pc = cl; pc && *pc; pc++)
            if (pc[0]=='n' && pc[1]=='o' && pc[2]=='d' && pc[3]=='h' && pc[4]=='c' && pc[5]=='p' && (pc[6]==' ' || pc[6]==0)) { net_nodhcp = 1; break; }
        for (const char *pc = cl; pc && *pc; pc++)
            if (pc[0]=='d' && pc[1]=='r' && pc[2]=='u' && pc[3]=='n' && pc[4]=='k' && (pc[5]==' ' || pc[5]==0)) { extern void window_set_drunk(int); window_set_drunk(1); serial_puts("drunk\n"); break; }
        ring3app_autoopen_arm(cl); r3stress_arm(cl); /* `open=keyrate` / `open=toroid` boot flag */
        for (; cl && *cl; cl++) {
            if (cl[0]=='w' && cl[1]=='x' && cl[2]=='h' && cl[3]=='o' && cl[4]=='s' && cl[5]=='t' && cl[6]=='=') {
                cl += 7; int hp = 0;
                while (((*cl >= '0' && *cl <= '9') || *cl == '.') && hp < 19) wx_override_host[hp++] = *cl++;
                wx_override_host[hp] = 0;
                if (*cl == ':') { unsigned int pt = 0; cl++; while (*cl >= '0' && *cl <= '9') pt = pt * 10 + (unsigned int)(*cl++ - '0'); if (pt && pt < 65536) wx_override_port = (unsigned short)pt; }
                serial_puts("wxhost="); serial_puts(wx_override_host); serial_puts("\n");
                break;
            }
        }
        /* 1.1.1: tilehost=A.B.C.D[:PORT], the wall_fetch equivalent of
           wxhost= right above -- see tile_override_host's own comment. */
        for (const char *pc = cl0; pc && *pc; pc++)
            if (pc[0]=='t' && pc[1]=='i' && pc[2]=='l' && pc[3]=='e' && pc[4]=='h' && pc[5]=='o' && pc[6]=='s' && pc[7]=='t' && pc[8]=='=') {
                pc += 9; int hp = 0;
                while (((*pc >= '0' && *pc <= '9') || *pc == '.') && hp < 19) tile_override_host[hp++] = *pc++;
                tile_override_host[hp] = 0;
                if (*pc == ':') { unsigned int pt = 0; pc++; while (*pc >= '0' && *pc <= '9') pt = pt * 10 + (unsigned int)(*pc++ - '0'); if (pt && pt < 65536) tile_override_port = (unsigned short)pt; }
                serial_puts("tilehost="); serial_puts(tile_override_host); serial_puts("\n");
                break;
            }
        for (const char *pc = cl0; pc && *pc; pc++)
            if (pc[0]=='w' && pc[1]=='a' && pc[2]=='l' && pc[3]=='l' && pc[4]=='t' && pc[5]=='h' && pc[6]=='e' && pc[7]=='m' && pc[8]=='e' && pc[9]=='=') {
                pc += 10;
                if (pc[0]=='m' && pc[1]=='a' && pc[2]=='p') wall_theme_override = WALL_WARM;
                else if (pc[0]=='s' && pc[1]=='a' && pc[2]=='t') wall_theme_override = WALL_SAT;
                else if (pc[0]=='p' && pc[1]=='h' && pc[2]=='o') wall_theme_override = WALL_PHOTO;
                else if (pc[0]=='g' && pc[1]=='r' && pc[2]=='e') wall_theme_override = WALL_GREY;
                break;
            }
        /* 1.0.12: llmhost=HOST / llmport=PORT, the Chat equivalent of
           wxhost= above -- for tools/checks/chat-samantha-check.py to point
           chat_send at a local fake HTTP server instead of the real,
           internet-facing Turing default, headlessly and without touching
           SETTINGS.TXT. Unlike wxhost's dotted-quad-only host, this takes
           any hostname byte up to the next space or end of string, the
           same real hostnames (turing.jaybulb.com) llm_host itself
           already accepts, not just an IP literal. Applied to llm_host/
           llm_port after settings_load() runs, not here -- see that call
           site for why. */
        for (const char *pc = cl0; pc && *pc; pc++)
            if (pc[0]=='l' && pc[1]=='l' && pc[2]=='m' && pc[3]=='h' && pc[4]=='o' && pc[5]=='s' && pc[6]=='t' && pc[7]=='=') {
                pc += 8; int hp = 0;
                while (*pc && *pc != ' ' && hp < LLM_HOST_MAX - 1) llm_host_override[hp++] = *pc++;
                llm_host_override[hp] = 0;
                serial_puts("llmhostcli="); serial_puts(llm_host_override); serial_puts("\n");
                break;
            }
        for (const char *pc = cl0; pc && *pc; pc++)
            if (pc[0]=='l' && pc[1]=='l' && pc[2]=='m' && pc[3]=='p' && pc[4]=='o' && pc[5]=='r' && pc[6]=='t' && pc[7]=='=') {
                pc += 8; unsigned int pt = 0;
                while (*pc >= '0' && *pc <= '9') pt = pt * 10 + (unsigned int)(*pc++ - '0');
                if (pt && pt < 65536) llm_port_override = (int)pt;
                break;
            }
        for (const char *pc = cl0; pc && *pc; pc++) {
            if (pc != cl0 && pc[-1] != ' ') continue; /* whole words only */
            int is_h = key_is(pc, 11, "claudehost="), is_t = key_is(pc, 12, "claudetoken="), is_p = key_is(pc, 11, "claudeport=");
            if (is_h || is_t) {
                char *dst = is_h ? claude_host_override : claude_token_override;
                int cap = is_h ? CLAUDE_HOST_MAX : CLAUDE_TOKEN_MAX, j = 0;
                const char *v = pc + (is_h ? 11 : 12);
                while (*v && *v != ' ' && j < cap - 1) dst[j++] = *v++;
                dst[j] = 0;
                if (is_h) { serial_puts("claudehostcli="); serial_puts(claude_host_override); serial_puts("\n"); }
                else serial_puts("claudetokencli=set\n");
            } else if (is_p) {
                unsigned int pt = 0; const char *v = pc + 11;
                while (*v >= '0' && *v <= '9' && pt < 100000) pt = pt * 10 + (unsigned int)(*v++ - '0');
                if (pt && pt < 65536) claude_port_override = (int)pt;
            }
        }
        stocks_cmdline(cl0); /* stkhost=HOST:PORT, kernel/stocks.h */
        jt_clip_cmdline(cl0); /* cliptrace, kernel/syscall.c: content hash on the CLIPCOPY/CLIPPASTE lines */
        jt_facehost_cmdline(cl0); /* facehost=HOST[:PORT], kernel/syscall.c: where ring-3 Samantha fetches her face and speech */
        uitext_cmdline(cl0); /* fontbreak=NAME, kernel/uitext.c: one face fed a blob that is not a font */
    }
    vga_text_mode_init(); /* real hardware/QEMU already boot into text mode via their own BIOS; a BIOS-less multiboot path (v86) never sets it at all, so make it explicit rather than inherited */
    klog("vga_text_mode_init: text mode 3 programmed");
    gdt_install();
    klog("gdt_install: GDT loaded");
    idt_install();
    klog("idt_install: IDT loaded");
    if (panic_test_at_boot) jt_panic_test_target(); /* never returns: ring-0 fault, isr_handler halts after printing the crash report */
    syscall_install(); /* v64: int 0x80 gate, DPL 3 */
    klog("syscall_install: int 0x80 gate live");
    irq_install();
    klog("irq_install: PIC remapped, PIT/keyboard IRQs live");
    mouse_init();
    klog("mouse_init: PS/2 mouse enabled");
    /* v62: probe the VMware absolute-pointer backdoor (port 0x5658). v86
       and QEMU's default pc machine both answer; bare hardware and
       -machine vmport=off don't, and PS/2 relative stays the only mouse. */
    klog(vmmouse_init() ? "vmmouse_init: VMware backdoor answered, absolute pointer on"
                        : "vmmouse_init: no backdoor, PS/2 relative pointer only");
    font_init(); /* must run while still in plain VGA text mode, before any window_open */
    klog("font_init: CP437 glyphs dumped from VGA hardware");
    pmm_init(multiboot_info_addr);
    klog("pmm_init: physical memory map parsed");
    paging_install();
    klog("paging_install: higher-half paging active");
    tasks_init();
    syscall_windows_init(); /* 1.9.23: compositor window rows start empty */
    klog("tasks_init: scheduler ready");
    klog(sb16_init() ? "sb16_init: Sound Blaster 16 found" : "sb16_init: no sound card");
    ata_blockdev_register(); /* v33 (0.33.0): register real backends before anything tries to mount a filesystem over one */
    ramdisk_init();
    trash_init();
    int fs_ok = fat_mount();
    fs_ok_global = fs_ok;
    klog(fs_ok ? "fat_mount: FAT16 filesystem mounted" : "fat_mount: no filesystem found");
    fat_vfs_register(); /* registered regardless of fs_ok: an unmounted fat backend just returns real failures, same as before v29 */
    ramfs_init();
    klog(uitext_fonts_preload() == TTF_FACE_COUNT ? "fonts: every face loaded" : "fonts: a face failed, drawn as DejaVu Sans");   /* 4.27.0 */
    klog("vfs: fat + ramfs backends registered, fat active");
    /* v86 (0.71.0): real root cause of "Files shows no files" on the
       browser demo, confirmed by reading vfs_register (drivers/vfs.c):
       the FIRST backend registered wins by default (fat, line above), and
       fat_mount() only ever finds a real disk when native QEMU boots off
       dotfiles.img; v86 has no disk image wired into its boot path at
       all (nothing in landing/v86/embed.js ever attaches a virtual disk),
       so fs_ok is always 0 there and Files/`ls` genuinely have nothing to
       show, not a bug in the FAT code itself. Native boots with a real
       disk are completely unaffected: this only swaps the default
       backend when fat_mount() itself already reported failure, which on
       real hardware/QEMU-with-a-disk it doesn't. Seed a few real demo
       files so a browser visitor sees something real to click, same
       honesty bar as everything else on this page (no fake data, no
       recording): a short note that names this exact fact. */
    if (!fs_ok) {
        static const char demo_readme[] =
            "This is a live demo.\n\n"
            "No real disk is attached in your browser (v86 has no way to\n"
            "mount the native dotfiles.img this kernel boots from on real\n"
            "hardware), so these are ramfs files, kept in memory only for\n"
            "this tab. Try the Terminal app: ls, cat README.TXT, echo.\n";
        static const char demo_notes[] =
            "Joshua Tree\n\n"
            "A freestanding kernel, built from scratch.\n\n"
            "This desktop, the file manager, mail, calendar, terminal,\n"
            "even this note you're reading, all real, all running on that\n"
            "kernel right now in your browser.\n";
        vfs_switch("ramfs"); /* switch first: vfs_write_file always targets the active backend, and fat's own write would just fail with no disk anyway */
        vfs_write_file("README.TXT", demo_readme, strlen(demo_readme));
        vfs_write_file("NOTES.TXT", demo_notes, strlen(demo_notes)); ramfs_seed_demo_docs(); if (boot_to_samantha) { unsigned char hh, mi, wd, dom, mon; cmos_read_time_stable(&hh, &mi, &wd, &dom, &mon); ramfs_seed_demo_events(cmos(9), mon, dom); } /* sample itinerary dated from the RTC; a real folder, so the tour's Burrow scene has something to open */
        klog("vfs: no FAT disk (v86 has none to mount), switched default backend to ramfs with demo files");
    }
    /* Install the shell utility on either the mounted disk or the RAM fallback. */
    { unsigned char probe;
      if (vfs_read_file("WC.BIN", &probe, 1) < 0 &&
          ring3app_write_packed("WC.BIN", user_wc, USER_WC_CLEN, USER_WC_LEN, USER_WC_SUM, 0) < 0)
          klog("wc: could not install WC.BIN");
    }
    settings_load(); /* v47: real settings, saved defaults if SETTINGS.TXT doesn't exist yet */
    /* 1.0.12: apply llmhost=/llmport= AFTER settings_load(), not before --
       a boot-time command-line override has to win over whatever a real
       persisted SETTINGS.TXT says, the same way wxhost= always wins for
       weather because it feeds a variable settings_load() never touches
       at all. llm_host/llm_port ARE settings_load()'s own variables, so
       applying this any earlier would just get silently overwritten by a
       real settings file the moment one exists. Deliberately never
       settings_save()'d: this is a one-boot test override, not a change
       to what Settings remembers. */
    if (llm_host_override[0]) { int p = 0; while (llm_host_override[p] && p < LLM_HOST_MAX - 1) { llm_host[p] = llm_host_override[p]; p++; } llm_host[p] = 0; }
    if (llm_port_override) llm_port = llm_port_override;
    if (claude_host_override[0]) { int p = 0; while (claude_host_override[p]) { claude_host[p] = claude_host_override[p]; p++; } claude_host[p] = 0; }
    if (claude_token_override[0]) { int p = 0; while (claude_token_override[p]) { claude_token[p] = claude_token_override[p]; p++; } claude_token[p] = 0; }
    if (claude_port_override) claude_port = claude_port_override;
    if (wall_theme_override >= 0) { wall_theme = wall_theme_override; serial_puts("wallthemeoverride="); { char d[2] = { (char)(48 + wall_theme), 0 }; serial_puts(d); } serial_puts("\n"); }
    clear();
    boot_chime();
    if (bench_at_boot) bench_run(fs_ok);
    puts("joshuatree v0 -- type help\n");
    if (!fs_ok) puts("(no FAT filesystem found -- ls/cat unavailable)\n");
    /* check.sh's only way to know the kernel reached this point: booting
       straight into gui_run() below switches the VGA card into a real
       graphics mode, which reprograms the Graphics Controller's memory-map
       select register, changing what physical address 0xB8000 even means.
       The old banner text is still real and still gets written above, it
       just becomes unreadable moments later once graphics mode takes over,
       confirmed directly (a real `xp` read after boot showed 0xffff, not
       the banner) rather than assumed. A plain RAM address ordinary text
       memory doesn't share survives the mode switch untouched. */
    *(volatile unsigned int *)0x9000 = 0xB007C0DE;
    kbd_drain(); /* discard any stray byte queued during boot (keyboard_enable_scanning, mouse_init) before real input starts */

    /* A real desktop OS boots to a desktop, not a command line: gui_run()
       already has a clean way back to this exact shell (esc closes the
       window, clears, prints "back in text mode", returns), so starting
       there instead of making every visitor type "gui" themselves is a
       straight improvement, not a special case for the browser demo. */
    gui_run();

    char line[80];
    for (;;) {
        puts("> ");
        int n = 0;
        for (;;) {
            char c = getch();
            if (c == '\n') { putc('\n'); break; }
            if (c == '\b') { if (n) { n--; putc('\b'); } continue; }
            if (n < (int)sizeof(line) - 1) { line[n++] = c; putc(c); }
        }
        line[n] = 0;
        shell_line(line);
    }
}
