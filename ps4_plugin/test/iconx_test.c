// PC test of the on-console icon extraction:
//   bash test/iconx_test.sh <archives dir> <out dir> <title id> <year code>
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <time.h>
#include "../source/iconx.h"

int g_trace;
void log_write(int sync, const char *fmt, ...)
{
    (void)sync;
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); fputc('\n', stderr);
}
uint64_t sceKernelGetProcessTime(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

int main(int argc, char **argv)
{
    if (argc < 5) { fprintf(stderr, "usage: iconx_test <archives> <out> <title> <year>\n"); return 2; }
    return iconx_extract_now(argv[1], argv[2], argv[3], atoi(argv[4])) == 0 ? 0 : 1;
}
