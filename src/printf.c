#include "printf.h"
#include "vga.h"
#include "serial.h"

// Use GCC's built-in varargs since we have no stdarg.h (-nostdinc).
// These expand to the right stack-walking code for our target.
typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_arg(ap, type)   __builtin_va_arg(ap, type)
#define va_end(ap)         __builtin_va_end(ap)

// Single output sink: every character printf emits goes through here.
// Writes to VGA (what you see in QEMU) and serial (what you see in the host
// terminal when running with -serial stdio). If you add another output
// later, this is the only place you need to change.
static void putchar_out(char c) {
    vga_putchar(c);
    serial_putc(c);
}

// Convert an unsigned integer to a string in the given base (2..16).
// Writes into `buf` (must be large enough — 33 bytes is safe for 32-bit).
// Returns the length written (not including null terminator).
static int utoa(unsigned int value, unsigned int base, char* buf, int uppercase) {
    const char* digits_lower = "0123456789abcdef";
    const char* digits_upper = "0123456789ABCDEF";
    const char* digits = uppercase ? digits_upper : digits_lower;

    // Special case: zero.
    if (value == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return 1;
    }

    // Write digits in reverse into a temp buffer, then flip.
    char tmp[33];
    int len = 0;
    while (value > 0) {
        tmp[len++] = digits[value % base];
        value /= base;
    }
    // Reverse into buf.
    for (int i = 0; i < len; i++) buf[i] = tmp[len - 1 - i];
    buf[len] = '\0';
    return len;
}

// Convert a signed int to decimal, handling negatives.
static int itoa(int value, char* buf) {
    if (value < 0) {
        buf[0] = '-';
        // Careful with INT_MIN: negating it overflows. Cast to unsigned first.
        unsigned int uv = (unsigned int)(-(value + 1)) + 1;
        return 1 + utoa(uv, 10, buf + 1, 0);
    }
    return utoa((unsigned int)value, 10, buf, 0);
}

// Print a null-terminated string, return count written.
static int print_str(const char* s) {
    int n = 0;
    while (*s) { putchar_out(*s++); n++; }
    return n;
}

int printf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);

    int written = 0;
    char buf[33];

    for (const char* p = fmt; *p; p++) {
        if (*p != '%') {
            putchar_out(*p);
            written++;
            continue;
        }

        // Look at the character after '%'.
        p++;
        switch (*p) {
            case 'd':
            case 'i': {
                int v = va_arg(ap, int);
                itoa(v, buf);
                written += print_str(buf);
                break;
            }
            case 'u': {
                unsigned int v = va_arg(ap, unsigned int);
                utoa(v, 10, buf, 0);
                written += print_str(buf);
                break;
            }
            case 'x': {
                unsigned int v = va_arg(ap, unsigned int);
                utoa(v, 16, buf, 0);
                written += print_str(buf);
                break;
            }
            case 'X': {
                unsigned int v = va_arg(ap, unsigned int);
                utoa(v, 16, buf, 1);
                written += print_str(buf);
                break;
            }
            case 'p': {
                // Pointer: print as 0x followed by hex.
                unsigned int v = (unsigned int)(unsigned long)va_arg(ap, void*);
                written += print_str("0x");
                utoa(v, 16, buf, 0);
                written += print_str(buf);
                break;
            }
            case 's': {
                const char* s = va_arg(ap, const char*);
                if (!s) s = "(null)";
                written += print_str(s);
                break;
            }
            case 'c': {
                // char is promoted to int when passed through varargs.
                char c = (char)va_arg(ap, int);
                putchar_out(c);
                written++;
                break;
            }
            case '%': {
                putchar_out('%');
                written++;
                break;
            }
            case '\0': {
                // Trailing '%' with nothing after it: print it and stop.
                putchar_out('%');
                written++;
                va_end(ap);
                return written;
            }
            default: {
                // Unknown specifier: print it literally so bugs are visible.
                putchar_out('%');
                putchar_out(*p);
                written += 2;
                break;
            }
        }
    }

    va_end(ap);
    return written;
}