#include "stdio.h"
#include "syscalls.h"

static const char* names[16] = {
    "BLACK",        "BLUE",         "GREEN",        "CYAN",
    "RED",          "MAGENTA",      "BROWN",        "LIGHT GREY",
    "DARK GREY",    "LIGHT BLUE",   "LIGHT GREEN",  "LIGHT CYAN",
    "LIGHT RED",    "LIGHT MAGENTA","YELLOW",       "WHITE",
};

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    printf("VGA color palette:\n");
    for (int i = 0; i < 16; i++) {
        int bg = (i == 0) ? COLOR_LIGHT_GREY : COLOR_BLACK;
        set_color(i, bg);
        // Manually pad to 2 chars since our printf doesn't do %2d.
        if (i < 10) putchar(' ');
        printf("  %d %s\n", i, names[i]);
    }
    reset_color();
    return 0;
}