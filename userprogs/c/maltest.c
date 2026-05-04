#include "stdio.h"
#include "stdlib.h"

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    printf("malloc test\n");

    // Many small allocations.
    char* arr[100];
    for (int i = 0; i < 100; i++) {
        arr[i] = (char*)malloc(64);
        if (!arr[i]) { printf("FAIL: malloc %d returned NULL\n", i); return 1; }
        for (int j = 0; j < 64; j++) arr[i][j] = (char)(i + j);
    }

    // Verify contents (catches accidental aliasing).
    for (int i = 0; i < 100; i++) {
        for (int j = 0; j < 64; j++) {
            if (arr[i][j] != (char)(i + j)) {
                printf("FAIL: corruption at arr[%d][%d]\n", i, j);
                return 1;
            }
        }
    }

    // Free every other one.
    for (int i = 0; i < 100; i += 2) free(arr[i]);

    // Big allocation that forces a fresh sbrk grow.
    char* big = (char*)malloc(8192);
    if (!big) { printf("FAIL: big malloc\n"); return 1; }
    for (int i = 0; i < 8192; i++) big[i] = 0x42;
    free(big);

    // Free the rest.
    for (int i = 1; i < 100; i += 2) free(arr[i]);

    printf("OK: malloc test passed\n");
    return 0;
}