#include "string.h"

size_t strlen(const char* s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char* a, const char* b) {
    while (*a && (*a == *b)) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char* a, const char* b, size_t n) {
    while (n && *a && (*a == *b)) { a++; b++; n--; }
    if (n == 0) return 0;
    return (unsigned char)*a - (unsigned char)*b;
}

void* memset(void* dst, int value, size_t n) {
    unsigned char* p = (unsigned char*)dst;
    for (size_t i = 0; i < n; i++) p[i] = (unsigned char)value;
    return dst;
}

void* memcpy(void* dst, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

unsigned int atou(const char* s) {
    // Skip leading whitespace (just spaces — we don't have isspace).
    while (*s == ' ') s++;

    unsigned int n = 0;
    while (*s >= '0' && *s <= '9') {
        n = n * 10 + (unsigned int)(*s - '0');
        s++;
    }
    return n;
}