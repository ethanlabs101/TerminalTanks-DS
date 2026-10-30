/* DS self-test ROM: runs the parity probe (tests/parity/probe.c) on the emulated ARM9 for the cases in cases.h and
 * writes its output, byte by byte, to the no$gba/melonDS debug character port. See gen_cases.py / run.py. */
#include <nds.h>
#include <stdarg.h>
#include <stdio.h>

static void dbg_putc(char c) { *(volatile uint32_t *)0x04FFFA1C = (uint8_t)c; }
static void dbg_puts(const char *s) { while (*s) dbg_putc(*s++); }
static int ds_printf(const char *fmt, ...) {
    static char tmp[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    dbg_puts(tmp);
    return n;
}
static int ds_putchar(int c) { dbg_putc((char)c); return c; }

#define printf ds_printf
#undef putchar
#define putchar ds_putchar
#define main probe_main
#include "../parity/probe.c"
#undef main
#undef printf
#undef putchar
#include "cases.h"

int main(void) {
    dbg_puts("\nSELFTEST_BEGIN\n");
    for (int i = 0; i < NCASES; i++) {
        int argc = 0;
        while (CASES[i][argc]) argc++;
        probe_main(argc, (char **)CASES[i]);
        dbg_puts("CASE_END\n");
    }
    dbg_puts("SELFTEST_DONE\n");
    for (;;) swiWaitForVBlank();
}
