#ifndef PRINTF_H
#define PRINTF_H

// Minimal printf supporting: %d, %u, %x, %X, %s, %c, %p, %%
// No width/precision/flags yet — those come later if needed.
int printf(const char* fmt, ...);

#endif