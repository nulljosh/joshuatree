#ifndef RING3_H
#define RING3_H
/* Ring-3 user-mode tasks (v3 one-shot demo, v64 real lifecycle): copies a
   tiny hand-written payload onto a user-accessible page and runs it as a
   scheduled ring-3 task via task_create_user(). Three payloads, picked by
   `mode`: "" writes a line through the int 0x80 SYS_WRITE gate, records
   the kernel's return value in a marker, then SYS_EXITs with 42; "fault"
   writes the marker then tries a privileged instruction and gets reaped
   by idt.c's ring-3 exception path; "spin" counts forever at ring 3 until
   task_kill() ends it. Each waits for its task to be reaped and reports,
   the shell comes back every time, no reboot. */
void ring3_test(const char *mode);
#endif
