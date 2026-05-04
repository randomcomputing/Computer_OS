#include "stdio.h"
#include "syscalls.h"

// Interactive line-echo. Reads lines from stdin and prints them back.
// Empty line exits. Once we have argv (A3) and file I/O (A4), this
// will grow into real `cat <file>` — for now it's a useful stand-in
// and a good test of the read/write round-trip.

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    printf("cat: type lines and they'll be echoed. Empty line to quit.\n");

    char buf[256];
    while (1) {
        int n = gets_safe(buf, sizeof(buf));
        if (n <= 0) break;
        puts(buf);
    }
    return 0;
}