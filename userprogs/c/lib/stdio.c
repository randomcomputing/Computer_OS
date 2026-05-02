#include "stdio.h"
#include "syscalls.h"

// GCC's built-in varargs (no stdarg.h available since we're -nostdinc).
typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_arg(ap, type)   __builtin_va_arg(ap, type)
#define va_end(ap)         __builtin_va_end(ap)

// =====================================================================
// Output buffering
//
// Each printf() syscall has nontrivial cost (int 0x80, ring transition,
// scheduler peek). One syscall per character makes printf("%d", n)
// painfully slow. We buffer into a small stack-local buffer and flush
// on '\n', when the buffer fills, or at the end of printf.
//
// The buffer is sized to fit a typical printf line in one syscall but
// stays small enough to live comfortably on a 4 KB stack. Each printf
// call gets its own buffer — no global state, so concurrent calls (we
// don't have those yet, but might) won't trample each other.
// =====================================================================

#define PRINTF_BUF 128

typedef struct {
    char buf[PRINTF_BUF];
    int  len;
    int  written;     // total chars written so far (return value)
} pbuf_t;

static void pbuf_flush(pbuf_t* p) {
    if (p->len > 0) {
        write(1, p->buf, (size_t)p->len);
        p->len = 0;
    }
}

static void pbuf_putc(pbuf_t* p, char c) {
    if (p->len >= PRINTF_BUF) pbuf_flush(p);
    p->buf[p->len++] = c;
    p->written++;
    // Flush on newline so output appears promptly when a program is
    // line-oriented (interactive prompts, progress lines, etc.).
    if (c == '\n') pbuf_flush(p);
}

static void pbuf_puts(pbuf_t* p, const char* s) {
    while (*s) pbuf_putc(p, *s++);
}

// =====================================================================
// Number formatting
// =====================================================================

static int utoa(unsigned int value, unsigned int base, char* buf, int uppercase) {
    const char* digits_lower = "0123456789abcdef";
    const char* digits_upper = "0123456789ABCDEF";
    const char* digits = uppercase ? digits_upper : digits_lower;

    if (value == 0) { buf[0] = '0'; buf[1] = '\0'; return 1; }

    char tmp[33];
    int len = 0;
    while (value > 0) {
        tmp[len++] = digits[value % base];
        value /= base;
    }
    for (int i = 0; i < len; i++) buf[i] = tmp[len - 1 - i];
    buf[len] = '\0';
    return len;
}

static int itoa(int value, char* buf) {
    if (value < 0) {
        buf[0] = '-';
        // Careful with INT_MIN: -INT_MIN overflows in two's complement.
        // The trick: -(v+1) is safe, then add one to the unsigned result.
        unsigned int uv = (unsigned int)(-(value + 1)) + 1;
        return 1 + utoa(uv, 10, buf + 1, 0);
    }
    return utoa((unsigned int)value, 10, buf, 0);
}

// =====================================================================
// printf
// =====================================================================

int printf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);

    pbuf_t p = { .buf = {0}, .len = 0, .written = 0 };
    char numbuf[33];

    for (const char* s = fmt; *s; s++) {
        if (*s != '%') { pbuf_putc(&p, *s); continue; }

        s++;
        switch (*s) {
            case 'd':
            case 'i': {
                int v = va_arg(ap, int);
                itoa(v, numbuf);
                pbuf_puts(&p, numbuf);
                break;
            }
            case 'u': {
                unsigned int v = va_arg(ap, unsigned int);
                utoa(v, 10, numbuf, 0);
                pbuf_puts(&p, numbuf);
                break;
            }
            case 'x': {
                unsigned int v = va_arg(ap, unsigned int);
                utoa(v, 16, numbuf, 0);
                pbuf_puts(&p, numbuf);
                break;
            }
            case 'X': {
                unsigned int v = va_arg(ap, unsigned int);
                utoa(v, 16, numbuf, 1);
                pbuf_puts(&p, numbuf);
                break;
            }
            case 'p': {
                unsigned int v = (unsigned int)(unsigned long)va_arg(ap, void*);
                pbuf_puts(&p, "0x");
                utoa(v, 16, numbuf, 0);
                pbuf_puts(&p, numbuf);
                break;
            }
            case 's': {
                const char* arg = va_arg(ap, const char*);
                if (!arg) arg = "(null)";
                pbuf_puts(&p, arg);
                break;
            }
            case 'c': {
                char c = (char)va_arg(ap, int);
                pbuf_putc(&p, c);
                break;
            }
            case '%':
                pbuf_putc(&p, '%');
                break;
            case '\0':
                // Trailing '%' with nothing after — print it and stop.
                pbuf_putc(&p, '%');
                pbuf_flush(&p);
                va_end(ap);
                return p.written;
            default:
                // Unknown specifier — print it literally so the bug is visible.
                pbuf_putc(&p, '%');
                pbuf_putc(&p, *s);
                break;
        }
    }

    pbuf_flush(&p);
    va_end(ap);
    return p.written;
}

// =====================================================================
// puts / putchar / getchar / gets_safe
// =====================================================================

int puts(const char* s) {
    int n = 0;
    while (s[n]) n++;
    write(1, s, (size_t)n);
    write(1, "\n", 1);
    return n + 1;
}

int putchar(int c) {
    char ch = (char)c;
    write(1, &ch, 1);
    return c;
}

int getchar(void) {
    char c;
    int n = read(0, &c, 1);
    if (n != 1) return -1;
    return (unsigned char)c;
}

int gets_safe(char* buf, size_t cap) {
    if (cap == 0) return -1;
    int n = read(0, buf, cap - 1);
    if (n < 0) return -1;
    // Strip a trailing newline if present (sys_read includes it).
    if (n > 0 && buf[n - 1] == '\n') n--;
    buf[n] = '\0';
    return n;
}