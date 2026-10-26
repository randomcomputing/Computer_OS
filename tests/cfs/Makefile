# Host tests for the CFS core (Phase 1) and journaling (Phase 2).
#
#   make -C tests/cfs        # build and run both
#   make -C tests/cfs clean

CC     ?= cc
# -fno-builtin silences host-only builtin/size_t mismatch notes against the
# kernel's own string.h; the kernel build never sees them.
CFLAGS ?= -std=c11 -Wall -Wextra -fno-builtin -O1 -g -I../../include

all: test_cfs test_journal test_fsck test_perms

test_cfs: test_cfs.c ../../src/cfs.c ../../include/cfs.h
	$(CC) $(CFLAGS) test_cfs.c ../../src/cfs.c -o test_cfs
	./test_cfs

test_journal: test_journal.c ../../src/cfs.c ../../include/cfs.h
	$(CC) $(CFLAGS) test_journal.c ../../src/cfs.c -o test_journal
	./test_journal

test_fsck: test_fsck.c ../../src/cfs.c ../../include/cfs.h
	$(CC) $(CFLAGS) test_fsck.c ../../src/cfs.c -o test_fsck
	./test_fsck

test_perms: test_perms.c ../../src/cfs.c ../../include/cfs.h
	$(CC) $(CFLAGS) test_perms.c ../../src/cfs.c -o test_perms
	./test_perms

clean:
	rm -f test_cfs test_journal test_fsck test_perms

.PHONY: all clean