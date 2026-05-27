#include "boot_banner.h"
#include "console.h"
#include "printf.h"

void boot_banner(void) {
    con_set_color(CON_WHITE, CON_BLACK);
    printf("###############################\n");
    printf("###");
    con_set_color(CON_LIGHT_CYAN, CON_BLACK);
    printf("       Computer OS       ");
    con_set_color(CON_WHITE, CON_BLACK);
    printf("###\n");
    printf("###############################\n");
    printf("\n");
}

void print_status(int ok, const char* text) {
    if (ok) {
        con_set_color(CON_LIGHT_GREEN, CON_BLACK);
        printf("[OK]");
    } else {
        con_set_color(CON_LIGHT_RED, CON_BLACK);
        printf("[!!]");
    }
    con_set_color(CON_WHITE, CON_BLACK);
    printf(" %s\n", text);
}