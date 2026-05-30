#ifndef STRING_H
#define STRING_H

#include "stdint.h"

size_t strlen(const char* s);
int    strcmp(const char* a, const char* b);
int    strncmp(const char* a, const char* b, size_t n);
void*  memset(void* dst, int value, size_t n);
void*  memcpy(void* dst, const void* src, size_t n);

unsigned int atou(const char* s);

#endif