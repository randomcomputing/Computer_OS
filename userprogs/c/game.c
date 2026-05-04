#include "stdio.h"
#include "stdlib.h"
#include "syscalls.h"

// Number-guessing game. Picks a "secret" number 1-100, prompts the user
// to guess, gives "higher" / "lower" hints, counts attempts. Quits on
// EOF or after the answer is found.
//
// We don't have a real RNG yet, so we seed a small linear congruential
// generator with getpid() XOR'd with a couple of stack addresses — good
// enough to feel non-deterministic between runs. The kernel hands out
// monotonically increasing PIDs, so successive `run game.elf` calls
// will at least pick different starting values.

static unsigned int rng_state;

static void rng_seed(unsigned int s) {
    // Make sure we never start at 0 — that would make the LCG stuck.
    rng_state = s ? s : 0xDEADBEEFu;
}

static unsigned int rng_next(void) {
    // Numerical Recipes LCG. Plenty for a guessing game.
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state;
}

// Read a line from stdin and try to parse it as a non-negative decimal.
// Returns -1 on parse failure or empty input.
static int read_int(void) {
    char buf[32];
    int n = gets_safe(buf, sizeof(buf));
    if (n <= 0) return -1;

    int value = 0;
    int saw_digit = 0;
    int i = 0;
    // Skip leading spaces.
    while (buf[i] == ' ' || buf[i] == '\t') i++;
    while (buf[i] >= '0' && buf[i] <= '9') {
        value = value * 10 + (buf[i] - '0');
        saw_digit = 1;
        i++;
    }
    return saw_digit ? value : -1;
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    int seed_anchor;     // address-of-local — different every run
    rng_seed((unsigned int)getpid() ^ (unsigned int)(unsigned long)&seed_anchor);

    int secret = (int)(rng_next() % 100u) + 1;     // 1..100 inclusive
    int attempts = 0;

    set_color(COLOR_LIGHT_CYAN, COLOR_BLACK);
    printf("\n=== Number Guessing ===\n");
    reset_color();
    printf("I'm thinking of a number between 1 and 100.\n");
    printf("Type your guess and hit enter. Empty line to give up.\n\n");

    while (1) {
        printf("guess> ");
        int guess = read_int();
        if (guess < 0) {
            printf("Giving up. The number was %d.\n", secret);
            return 1;
        }
        attempts++;

        if (guess == secret) {
            set_color(COLOR_LIGHT_GREEN, COLOR_BLACK);
            printf("Correct! ");
            reset_color();
            printf("You got it in %d attempt%s.\n",
                   attempts, attempts == 1 ? "" : "s");
            return 0;
        } else if (guess < secret) {
            printf("  higher\n");
        } else {
            printf("  lower\n");
        }
    }
}