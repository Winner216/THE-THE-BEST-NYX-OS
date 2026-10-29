// user/fault.c  –  deliberate ring-3 page fault.
// Proof that user mode is SAFE: dereferencing NULL raises #PF from
// ring 3, the kernel kills ONLY this task (see the USER FAULT path in
// cpu/isr.c) and the OS — desktop, other tasks — keeps running.
#include "userlib.h"

void _start(void) {
    sys_write("USERF fault program starting (pid=", 37);
    {
        char b[12]; int p = 0; unsigned v = (unsigned)sys_getpid();
        if (v == 0) { b[p++] = '0'; } else {
            char t[12]; int q = 0;
            while (v > 0) { t[q++] = (char)('0' + v % 10); v /= 10; }
            while (q > 0) b[p++] = t[--q];
        }
        b[p] = 0;
        sys_write(b, p);
    }
    sys_write(")\n", 2);

    sys_sleep(10);                       // let the message flush

    volatile unsigned* p = (volatile unsigned*)0x0;
    sys_write("USERF about to dereference NULL\n", 33);
    *p = 42;                             // #PF (page fault) from ring 3

    // Never reached: the task is dead, the OS is not.
    sys_write("USERF survived?! (should not print)\n", 35);
    sys_exit();
}
