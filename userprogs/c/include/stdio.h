#ifndef _USER_STDIO_H
#define _USER_STDIO_H

#include "syscalls.h"

// printf-style formatted output to stdout. Returns chars written.
// Supports: %d %i %u %x %X %p %s %c %%
// No width/precision/flags yet — bare conversions only.
int printf(const char* fmt, ...);

// Print a null-terminated string followed by a newline.
int puts(const char* s);

// Write a single character to stdout.
int putchar(int c);

// Read one character from stdin. Blocks until a key is pressed.
// Returns the character, or -1 on error.
int getchar(void);

// Read a line into `buf` (at most cap-1 chars), null-terminate. The
// trailing newline (if any) is stripped. Returns the number of chars
// stored, or -1 on error.
int gets_safe(char* buf, size_t cap);

#endif