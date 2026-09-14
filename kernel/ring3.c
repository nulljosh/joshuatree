/* Ring-3 tasks with a real lifecycle (v64, 0.61.0), see ring3.h. Payloads
   live in ring3_asm.S, hand-written on purpose since each has to be
   relocatable byte-for-byte onto a page it wasn't linked at. */
#include "ring3.h"
#include "paging.h"
#include "task.h"
#include "irq.h"
#include "console.h"
#include "serial.h"

typedef unsigned int u32;
typedef unsigned char u8;

static u8 ring3_code_page[4096]  __attribute__((aligned(4096)));
static u8 ring3_stack_page[4096] __attribute__((aligned(4096)));

extern u8 ring3_demo_code[],  ring3_demo_code_end[];
extern u8 ring3_fault_code[], ring3_fault_code_end[];
extern u8 ring3_spin_code[],  ring3_spin_code_end[];

#define KERNEL_VIRTUAL_BASE 0xC0000000u
#define MARKER (*(volatile u32 *)ring3_stack_page)

/* Copies a payload onto the user code page and starts it as a ring-3
   task. One code page and one stack page exist, so one ring-3 task at a
   time; the caller waits for it. The marker lives at the base of the
   already-mapped stack page, not a separate kernel global: a real early
   test (v3) wrote straight to a C-global marker instead and page-faulted
   immediately, since that global's own page was never marked user-
   accessible. Its address, plus the code page's own base (the only way a
   payload copied away from its link address can find its own data), go
   in as two words on the user stack, argv-style. */
static int ring3_start(const u8 *code, const u8 *code_end) {
    paging_set_user(ring3_code_page);
    paging_set_user(ring3_stack_page);
    u32 len = (u32)(code_end - code);
    for (u32 i = 0; i < len; i++) ring3_code_page[i] = code[i];
    MARKER = 0;
    u32 *usp = (u32 *)(ring3_stack_page + sizeof(ring3_stack_page));
    *(--usp) = (u32)ring3_code_page;  /* 4(%esp) at entry */
    *(--usp) = (u32)ring3_stack_page; /* 0(%esp) at entry: the marker */
    return task_create_user((u32)ring3_code_page, (u32)usp);
}

static void wait_reaped(int id) {
    while (task_used(id)) yield();
}

static void report_marker(void) {
    puts("marker at "); puthex((u32)ring3_stack_page);
    puts(" (phys "); puthex((u32)ring3_stack_page - KERNEL_VIRTUAL_BASE);
    puts(") = "); puthex(MARKER); puts("\n");
}

void ring3_test(const char *mode) {
    int fault = mode && mode[0] == 'f';
    int spin  = mode && mode[0] == 's';
    int id;
    if (spin) {
        id = ring3_start(ring3_spin_code, ring3_spin_code_end);
        if (id < 0) { puts("no free task slots\n"); return; }
        puts("ring-3 spinner started as task "); putc('0' + id); puts(", letting it run...\n");
        sleep_ticks(5);
        u32 a = MARKER;
        sleep_ticks(5);
        u32 b = MARKER;
        task_kill(id);
        wait_reaped(id);
        u32 c = MARKER;
        sleep_ticks(5);
        u32 d = MARKER;
        report_marker();
        int ran = b > a, stopped = d == c;
        puts(ran ? "ring 3 ran under preemption (counter moved): ok\n" : "ring 3 spinner never ran: FAILED\n");
        puts(stopped ? "kill: ring-3 task really stopped, slot reaped: ok\n" : "kill: FAILED (counter kept moving)\n");
        serial_puts(ran && stopped ? "ring3test spin: ok\n" : "ring3test spin: FAILED\n");
        return;
    }
    id = fault ? ring3_start(ring3_fault_code, ring3_fault_code_end)
               : ring3_start(ring3_demo_code,  ring3_demo_code_end);
    if (id < 0) { puts("no free task slots\n"); return; }
    puts(fault ? "ring-3 task started (will fault on a privileged instruction)...\n"
               : "ring-3 task started (will write via int 0x80, then exit)...\n");
    wait_reaped(id);
    report_marker();
    if (fault) {
        int ok = MARKER == 0xABCDEF01;
        puts(ok ? "ring 3 ran, faulted, and was reaped; kernel still alive: ok\n" : "ring 3 fault path: FAILED\n");
        serial_puts(ok ? "ring3test fault: ok\n" : "ring3test fault: FAILED\n");
    } else {
        int code = task_last_exit_code();
        puts("exit code "); puthex((u32)code); puts("\n");
        int ok = MARKER == 0xABCD001F && code == 42; /* 0x1F = 31 bytes, the message's length, returned by the kernel and written back from ring 3 */
        puts(ok ? "int 0x80 round trip (write returned 31, exit 42): ok\n" : "int 0x80 round trip: FAILED\n");
        serial_puts(ok ? "ring3test: ok\n" : "ring3test: FAILED\n");
    }
}
