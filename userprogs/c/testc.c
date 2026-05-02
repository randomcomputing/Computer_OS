// testc.c — user-space test suite for Computer_OS.
//
// Exercises the ELF address space: .text, .data, .bss, and stack.
// Each test prints PASS or FAIL so results are easy to scan.

#include "stdio.h"
#include "string.h"
#include "syscalls.h"

// ---- .data: initialized globals -------------------------------------
// These live in the ELF's PT_LOAD data segment. The loader must copy
// the file image here; wrong values mean the data segment wasn't loaded.

static int  g_magic   = 0xDEADBEEF;
static char g_greeting[] = "hello";

// ---- .bss: zero-initialized globals ---------------------------------
// memsz > filesz in the PT_LOAD segment. The loader zeros the gap;
// non-zero values mean BSS wasn't zeroed after loading.

#define BSS_SIZE 1024
static unsigned char g_zeros[BSS_SIZE];
static int           g_counter;        // must start at 0

// ---- helpers --------------------------------------------------------

static int tests_run  = 0;
static int tests_fail = 0;

static void check(const char* name, int ok) {
    tests_run++;
    if (ok) {
        set_color(COLOR_LIGHT_GREEN, COLOR_BLACK);
        printf("  PASS  ");
        reset_color();
        printf("%s\n", name);
    } else {
        set_color(COLOR_LIGHT_RED, COLOR_BLACK);
        printf("  FAIL  ");
        reset_color();
        printf("%s\n", name);
        tests_fail++;
    }
}

// ---- recursive Fibonacci (exercises the stack) ----------------------

static int fib(int n) {
    if (n <= 1) return n;
    return fib(n - 1) + fib(n - 2);
}

// ---- tests ----------------------------------------------------------

static void test_data_segment(void) {
    printf("data segment:\n");
    check(".data int  == 0xDEADBEEF", g_magic == (int)0xDEADBEEF);
    check(".data str  == \"hello\"",   strcmp(g_greeting, "hello") == 0);

    // Mutate and re-check to prove the page is writable.
    g_magic = 42;
    check(".data write-back",         g_magic == 42);
}

static void test_bss_segment(void) {
    printf("bss segment:\n");

    int all_zero = 1;
    for (int i = 0; i < BSS_SIZE; i++) {
        if (g_zeros[i] != 0) { all_zero = 0; break; }
    }
    check("g_zeros[1024] all zero",   all_zero);
    check("g_counter     == 0",       g_counter == 0);

    // Write then read back.
    for (int i = 0; i < BSS_SIZE; i++) g_zeros[i] = (unsigned char)(i & 0xFF);
    check("g_zeros write-back [0]",   g_zeros[0] == 0);
    check("g_zeros write-back [255]", g_zeros[255] == 255);
    check("g_zeros write-back [256]", g_zeros[256] == 0);
}

static void test_stack(void) {
    printf("stack / recursion:\n");
    // fib(10) = 55, fib(15) = 610 — enough frames to prove the stack works.
    check("fib(10) == 55",  fib(10) == 55);
    check("fib(15) == 610", fib(15) == 610);
}

static void test_strings(void) {
    printf("string library:\n");

    check("strlen(\"abc\") == 3", strlen("abc") == 3);
    check("strlen(\"\")    == 0", strlen("") == 0);

    check("strcmp equal",        strcmp("foo", "foo") == 0);
    check("strcmp less",         strcmp("abc", "abd") < 0);
    check("strcmp greater",      strcmp("z", "a") > 0);

    char buf[32];
    memset(buf, 0xAA, sizeof(buf));
    check("memset fills",        (unsigned char)buf[0] == 0xAA &&
                                 (unsigned char)buf[31] == 0xAA);

    memcpy(buf, "copied", 7);   // includes the null terminator
    check("memcpy copies",       strcmp(buf, "copied") == 0);
}

static void test_printf_formats(void) {
    printf("printf formats:\n");
    // These are visual checks — if the format is wrong the output looks off.
    printf("  %%d  : %d\n",  -42);
    printf("  %%u  : %u\n",  42u);
    printf("  %%x  : %x\n",  0xCAFE);
    printf("  %%X  : %X\n",  0xCAFE);
    printf("  %%s  : %s\n",  "world");
    printf("  %%c  : %c\n",  'Z');
    printf("  %%p  : %p\n",  (void*)0x01000000);
    check("printf ran without crashing", 1);
}

static void test_pid(void) {
    printf("syscalls:\n");
    int pid = getpid();
    check("getpid() > 0", pid > 0);
}

// ---- entry ----------------------------------------------------------

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    printf("=== Computer_OS user-space tests ===\n\n");

    test_data_segment();
    test_bss_segment();
    test_stack();
    test_strings();
    test_printf_formats();
    test_pid();

    printf("\n");
    if (tests_fail == 0) {
        set_color(COLOR_LIGHT_GREEN, COLOR_BLACK);
        printf("%d/%d passed - all good\n", tests_run, tests_run);
    } else {
        set_color(COLOR_LIGHT_RED, COLOR_BLACK);
        printf("%d/%d passed - %d FAILED\n",
               tests_run - tests_fail, tests_run, tests_fail);
    }
    reset_color();

    return tests_fail;
}
