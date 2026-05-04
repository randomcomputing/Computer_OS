#include "stdio.h"
#include "stdlib.h"
#include "syscalls.h"

// Tiny interactive integer calculator.
//
// Each line is an expression: `<num> <op> <num>` where op is + - * / %.
// Whitespace flexible. Empty line or "quit" exits. Examples:
//
//   calc> 5 + 3
//   = 8
//   calc> 100 / 7
//   = 14
//   calc> 99 * -2
//   = -198
//
// The parser is intentionally dumb — one binary op per line, no
// precedence, no parens. The point is to exercise read/parse/printf
// and give us a useful tool, not to write yacc.

// Skip spaces/tabs in p; advance *p past them. Returns 1 if anything
// remains, 0 if we hit end-of-string.
static int skip_ws(const char** p) {
    while (**p == ' ' || **p == '\t') (*p)++;
    return **p != '\0' && **p != '\n';
}

// Parse an optionally-signed decimal integer at *p; advance *p past it.
// Returns 1 on success (with *out set), 0 on failure.
static int parse_int(const char** p, int* out) {
    int sign = 1;
    if (**p == '-') { sign = -1; (*p)++; }
    else if (**p == '+') (*p)++;

    if (**p < '0' || **p > '9') return 0;
    int v = 0;
    while (**p >= '0' && **p <= '9') {
        v = v * 10 + (**p - '0');
        (*p)++;
    }
    *out = sign * v;
    return 1;
}

// Returns 1 if the string in `s` (any case) equals `target`.
static int streq_lower(const char* s, const char* target) {
    for (int i = 0; ; i++) {
        char a = s[i];
        if (a >= 'A' && a <= 'Z') a = a - 'A' + 'a';
        if (a != target[i]) return 0;
        if (a == '\0') return 1;
    }
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    set_color(COLOR_LIGHT_CYAN, COLOR_BLACK);
    printf("\n=== calc ===\n");
    reset_color();
    printf("Type expressions like `5 + 3`. Operators: + - * / %%\n");
    printf("Empty line or `quit` to exit.\n\n");

    char line[128];
    while (1) {
        printf("calc> ");
        int n = gets_safe(line, sizeof(line));
        if (n <= 0) break;                      // EOF or empty line
        if (streq_lower(line, "quit")) break;

        const char* p = line;
        if (!skip_ws(&p)) continue;             // whitespace-only line

        int a;
        if (!parse_int(&p, &a)) {
            printf("error: expected number\n");
            continue;
        }
        if (!skip_ws(&p)) {
            printf("= %d\n", a);                // bare number echoes
            continue;
        }

        char op = *p++;
        if (op != '+' && op != '-' && op != '*' && op != '/' && op != '%') {
            printf("error: expected one of + - * / %%\n");
            continue;
        }
        if (!skip_ws(&p)) {
            printf("error: expected number after '%c'\n", op);
            continue;
        }

        int b;
        if (!parse_int(&p, &b)) {
            printf("error: expected number\n");
            continue;
        }

        int result = 0;
        switch (op) {
            case '+': result = a + b; break;
            case '-': result = a - b; break;
            case '*': result = a * b; break;
            case '/':
                if (b == 0) { printf("error: divide by zero\n"); continue; }
                result = a / b;
                break;
            case '%':
                if (b == 0) { printf("error: modulo by zero\n"); continue; }
                result = a % b;
                break;
        }
        printf("= %d\n", result);
    }

    printf("bye\n");
    return 0;
}