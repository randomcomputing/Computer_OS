#ifndef STRING_H
#define STRING_H

typedef unsigned int size_t;

size_t strlen(const char* s);
int    strcmp(const char* a, const char* b);
int    strncmp(const char* a, const char* b, size_t n);
void*  memset(void* dst, int value, size_t n);
void*  memcpy(void* dst, const void* src, size_t n);

// Parse a non-negative decimal integer. Stops at the first non-digit.
// Returns 0 if no digits are found.
unsigned int atou(const char* s);

#endif