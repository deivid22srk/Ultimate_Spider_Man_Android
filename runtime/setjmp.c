/* setjmp/longjmp for recompiled code. The lifter emits a host setjmp in
 * the guest function that called _setjmp3; longjmp restores the guest
 * registers saved in the guest jmp_buf and jumps back to that frame. */
#include "rt.h"
#include <setjmp.h>

#define SLOTS 64
static __thread struct { uint32_t guest; jmp_buf jb; } t_slots[SLOTS];
static __thread int t_next;

jmp_buf *rt_setjmp_slot(CPU *c, uint32_t guest_jb) {
    (void)c;
    for (int i = 0; i < SLOTS; i++)
        if (t_slots[i].guest == guest_jb) return &t_slots[i].jb;
    int i = t_next++ % SLOTS;
    t_slots[i].guest = guest_jb;
    return &t_slots[i].jb;
}

void rt_setjmp3(CPU *c) {
    uint32_t esp = c->r[4];
    uint32_t jb = M32(esp + 4);
    M32(jb + 0) = c->r[5];          /* ebp */
    M32(jb + 4) = c->r[3];          /* ebx */
    M32(jb + 8) = c->r[7];          /* edi */
    M32(jb + 12) = c->r[6];         /* esi */
    M32(jb + 16) = esp + 4;         /* esp after return */
    M32(jb + 20) = M32(esp);        /* eip */
    M32(jb + 24) = M32(c->fs_base); /* SEH registration */
    M32(jb + 28) = 0xFFFFFFFFu;
    c->r[4] = esp + 4;
    c->r[0] = 0;
}

void rt_longjmp(CPU *c, uint32_t jb, uint32_t val) {
    jmp_buf *slot = NULL;
    for (int i = 0; i < SLOTS; i++)
        if (t_slots[i].guest == jb) slot = &t_slots[i].jb;
    if (!slot) rt_fatal("longjmp to unknown jmp_buf %08x", jb);
    c->r[5] = M32(jb + 0);
    c->r[3] = M32(jb + 4);
    c->r[7] = M32(jb + 8);
    c->r[6] = M32(jb + 12);
    c->r[4] = M32(jb + 16);
    c->r[0] = val ? val : 1;
    longjmp(*slot, 1);
}
