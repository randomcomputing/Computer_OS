# Host test for the CFS core (Phase 1).
#
#   make -C tests/cfs        # build and run
#   make -C tests/cfs clean

CC     ?= cc
# -fno-builtin silences host-only builtin/size_t mismatch notes against the
# kernel's own string.h; the kernel build never sees them.
CFLAGS ?= -std=c11 -Wall -Wextra -fno-builtin -O1 -g -I../../include

BIN = test_cfs

$(BIN): test_cfs.c ../../src/cfs.c ../../include/cfs.h
	$(CC) $(CFLAGS) test_cfs.c ../../src/cfs.c -o $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)

.PHONY: clean
