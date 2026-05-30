#include "printf.h"
#include "console.h"
#include "serial.h"
#include "stdint.h"

typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_arg(ap, type)   __builtin_va_arg(ap, type)
#define va_end(ap)         __builtin_va_end(ap)

static void putchar_out(char c) {
    con_putchar(c);
    serial_putc(c);
}

/* Convert a 64-bit unsigned integer to string. buf must be >= 65 bytes. */
static int utoa64(uint64_t value, unsigned int base, char* buf, int uppercase) {
    const char* digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    if (value == 0) { buf[0] = '0'; buf[1] = '\0'; return 1; }
    char tmp[65];
    int len = 0;
    while (value > 0) { tmp[len++] = digits[value % base]; value /= base; }
    for (int i = 0; i < len; i++) buf[i] = tmp[len - 1 - i];
    buf[len] = '\0';
    return len;
}

static int itoa(int value, char* buf) {
    if (value < 0) {
        buf[0] = '-';
        uint64_t uv = (uint64_t)(-(value + 1)) + 1;
        return 1 + utoa64(uv, 10, buf + 1, 0);
    }
    return utoa64((uint64_t)(unsigned int)value, 10, buf, 0);
}

static int print_str(const char* s) {
    int n = 0;
    while (*s) { putchar_out(*s++); n++; }
    return n;
}

int printf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int written = 0;
    char buf[65];

    for (const char* p = fmt; *p; p++) {
        if (*p != '%') { putchar_out(*p); written++; continue; }
        p++;

        /* Check for 'll' length modifier. */
        int is_ll = 0;
        if (*p == 'l' && *(p+1) == 'l') { is_ll = 1; p += 2; }
        else if (*p == 'l') { p++; }  /* single 'l' — treat same as ll on 64-bit */

        switch (*p) {
            case 'd':
            case 'i': {
                if (is_ll) {
                    long long v = va_arg(ap, long long);
                    if (v < 0) { putchar_out('-'); utoa64((uint64_t)(-(v+1))+1, 10, buf, 0); }
                    else utoa64((uint64_t)v, 10, buf, 0);
                    written += print_str(buf);
                } else {
                    int v = va_arg(ap, int);
                    itoa(v, buf); written += print_str(buf);
                }
                break;
            }
            case 'u': {
                uint64_t v = is_ll ? va_arg(ap, unsigned long long)
                                   : (uint64_t)va_arg(ap, unsigned int);
                utoa64(v, 10, buf, 0); written += print_str(buf); break;
            }
            case 'x': {
                uint64_t v = is_ll ? va_arg(ap, unsigned long long)
                                   : (uint64_t)va_arg(ap, unsigned int);
                utoa64(v, 16, buf, 0); written += print_str(buf); break;
            }
            case 'X': {
                uint64_t v = is_ll ? va_arg(ap, unsigned long long)
                                   : (uint64_t)va_arg(ap, unsigned int);
                utoa64(v, 16, buf, 1); written += print_str(buf); break;
            }
            case 'p': {
                uint64_t v = (uint64_t)va_arg(ap, void*);
                written += print_str("0x");
                utoa64(v, 16, buf, 0); written += print_str(buf); break;
            }
            case 's': {
                const char* s = va_arg(ap, const char*);
                if (!s) s = "(null)";
                written += print_str(s); break;
            }
            case 'c': {
                char c = (char)va_arg(ap, int);
                putchar_out(c); written++; break;
            }
            case '%': { putchar_out('%'); written++; break; }
            case '\0': { putchar_out('%'); written++; va_end(ap); return written; }
            default: { putchar_out('%'); putchar_out(*p); written += 2; break; }
        }
    }
    va_end(ap);
    return written;
}