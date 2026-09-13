/* Cooperative context switch: save the callee-saved registers ABI-required
   to survive a call, swap stack pointers, restore. A brand-new task's stack
   is fabricated to look exactly like this layout so the final `ret` here
   jumps straight into its entry function the first time it's switched to. */
.global switch_context
switch_context:
    push %ebp
    push %ebx
    push %esi
    push %edi
    mov 20(%esp), %eax   /* old_esp_ptr (arg1) */
    mov %esp, (%eax)
    mov 24(%esp), %esp   /* new_esp (arg2) */
    pop %edi
    pop %esi
    pop %ebx
    pop %ebp
    ret
