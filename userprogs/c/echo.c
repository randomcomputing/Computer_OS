// echo.c — interactive echo. Reads a line from stdin and echoes it.
//
// Demonstrates:
//   - SYS_READ via getchar/gets_safe
//   - building up state in user space
//   - exiting cleanly with a non-zero status (via Ctrl+? if we add it)
//
// Build:  make echo.bin
// Run:    > run echo.bin
//
// Type a line and press enter. The program will echo it back, then
// exit. Type "quit" to exit early without echoing.

#include "stdio.h"
#include "string.h"

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    printf("Type something (or 'quit'):\n");
    printf("> ");

    char line[128];
    int n = gets_safe(line, sizeof(line));
    if (n < 0) {
        printf("read failed\n");
        return 1;
    }

    if (strcmp(line, "quit") == 0) {
        printf("ok, quitting.\n");
        return 0;
    }

    printf("you said: \"%s\" (%d chars)\n", line, n);
    return 0;
}