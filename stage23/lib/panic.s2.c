#include <lib/print.h>
#include <lib/real.h>
#include <lib/trace.h>

__attribute__((noreturn)) void panic(const char *fmt, ...) {
    asm volatile ("cli" ::: "memory");

    va_list args;

    va_start(args, fmt);

    print("\033[31mPANIC\033[37;1m\033[0m: ");
    vprint(fmt, args);

    va_end(args);

    print("\n");
    print_stacktrace(NULL);

#if defined (bios)
    rm_hcf();
#elif defined (uefi)
    for (;;) asm ("hlt");
#endif
}
