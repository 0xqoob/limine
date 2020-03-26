#ifndef __LIB__REAL_H__
#define __LIB__REAL_H__

#include <stdint.h>

#define rm_seg(x) (unsigned short)(((int)x & 0xFFFF0) >> 4)
#define rm_off(x) (unsigned short)(((int)x & 0x0000F) >> 0)

#define rm_desegment(seg, off) (((uint32_t)(seg) << 4) + (uint32_t)(off))

#define EFLAGS_CF (1 << 0)

struct rm_regs {
    uint32_t eflags;
    uint32_t ebp;
    uint32_t edi;
    uint32_t esi;
    uint32_t edx;
    uint32_t ecx;
    uint32_t ebx;
    uint32_t eax;
};

void rm_int(uint8_t, struct rm_regs *, struct rm_regs *);

#endif
