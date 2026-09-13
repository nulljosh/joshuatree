/* One-shot ring-3 demo: see ring3.h. ring3_demo_code/enter_ring3 live in
   ring3.S, hand-written on purpose since the payload has to be relocatable
   byte-for-byte onto a page it wasn't linked at. */
#include "ring3.h"
#include "paging.h"
#include "console.h"

typedef unsigned int u32;
typedef unsigned char u8;

static u8 ring3_code_page[4096]  __attribute__((aligned(4096)));
static u8 ring3_stack_page[4096] __attribute__((aligned(4096)));

extern u8 ring3_demo_code[];
extern u8 ring3_demo_code_end[];
extern void enter_ring3(u32 eip, u32 esp, u32 marker_addr);

void ring3_test(void) {
    paging_set_user(ring3_code_page);
    paging_set_user(ring3_stack_page);

    u32 code_len = (u32)(ring3_demo_code_end - ring3_demo_code);
    for (u32 i = 0; i < code_len; i++) ring3_code_page[i] = ring3_demo_code[i];

    /* Marker lives at the base of the already-mapped stack page, not a
       separate kernel global: a real early test wrote straight to a
       C-global marker instead and it page-faulted immediately, since that
       global's own page was never marked user-accessible. Using a spot
       inside a page ring-3 already owns sidesteps needing a third mapping
       just for this. */
    puts("entering ring 3 (this halts the kernel when it faults, by design -- reboot after)...\n");
    enter_ring3((u32)ring3_code_page, (u32)(ring3_stack_page + sizeof(ring3_stack_page)), (u32)ring3_stack_page);
}
