#include <stddef.h>
#include <stdint.h>
#include <protos/chainload.h>
#include <lib/part.h>
#include <lib/config.h>
#include <lib/blib.h>
#include <drivers/disk.h>
#include <lib/term.h>
#include <mm/mtrr.h>

__attribute__((section(".realmode"), used))
static void spinup(uint8_t drive) {
    asm volatile (
        "cld\n\t"

        "jmp 0x08:1f\n\t"
        "1: .code16\n\t"
        "mov ax, 0x10\n\t"
        "mov ds, ax\n\t"
        "mov es, ax\n\t"
        "mov fs, ax\n\t"
        "mov gs, ax\n\t"
        "mov ss, ax\n\t"
        "mov eax, cr0\n\t"
        "and al, 0xfe\n\t"
        "mov cr0, eax\n\t"
        "jmp 0x0000:1f\n\t"
        "1:\n\t"
        "mov ax, 0\n\t"
        "mov ds, ax\n\t"
        "mov es, ax\n\t"
        "mov fs, ax\n\t"
        "mov gs, ax\n\t"
        "mov ss, ax\n\t"

        "sti\n\t"

        "push 0\n\t"
        "push 0x7c00\n\t"
        "retf\n\t"

        ".code32\n\t"
        :
        : "d" (drive)
        : "memory"
    );
}

void chainload(void) {
    int part; {
        char buf[32];
        if (!config_get_value(buf, 0, 32, "PARTITION")) {
            part = -1;
        } else {
            if (strtoui(buf, 0, 10) < 1 || strtoui(buf, 0, 10) > 256) {
                panic("BIOS partition number outside range 1-256");
            }
            part = (int)strtoui(buf, 0, 10);
        }
    }
    int drive; {
        char buf[32];
        if (!config_get_value(buf, 0, 32, "DRIVE")) {
            panic("DRIVE not specified");
        }
        if (strtoui(buf, 0, 10) < 1 || strtoui(buf, 0, 10) > 16) {
            panic("BIOS drive number outside range 1-16");
        }
        drive = (int)strtoui(buf, 0, 10);
    }

    term_deinit();

    if (part != -1) {
        struct part p;
        part_get(&p, drive, part);

        part_read(&p, (void *)0x7c00, 0, 512);
    } else {
        disk_read(drive, (void *)0x7c00, 0, 512);
    }

    mtrr_restore();

    spinup(drive);
}
