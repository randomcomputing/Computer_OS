# Makefile for Computer_OS C user programs.
#
# Each .c at the top level becomes an ELF32 executable that the kernel's
# ELF loader reads from the FAT12 disk.  Shared startup (crt0.s) and
# library code (libuser.a) are built from sources in lib/.

CC      = i686-elf-gcc
LD      = i686-elf-ld
AR      = i686-elf-ar
AS_NASM = nasm

# Host fallback: uncomment if you don't have the cross-toolchain.
# CC      = gcc
# LD      = ld
# AR      = ar
# CFLAGS_EXTRA = -m32
# LDFLAGS_EXTRA = -m elf_i386

CFLAGS = -ffreestanding -fno-asynchronous-unwind-tables -fno-pie \
         -fno-stack-protector -nostdlib -nostdinc -Iinclude \
         -O2 -Wall -Wextra $(CFLAGS_EXTRA)
LDFLAGS = -T user.ld $(LDFLAGS_EXTRA)
ARFLAGS = rcs

# Sources for the user-side libc.
LIB_C_SRCS  := lib/stdio.c lib/string.c lib/malloc.c
LIB_S_SRCS  := lib/crt0.s

LIB_C_OBJS  := $(LIB_C_SRCS:.c=.o)
LIB_S_OBJS  := $(LIB_S_SRCS:.s=.o)

CRT0    = lib/crt0.o
LIBUSER = libuser.a

PROGRAMS_C := $(wildcard *.c)
PROGRAMS   := $(PROGRAMS_C:.c=.elf)

.PHONY: all clean install
all: $(PROGRAMS)

%.elf: %.o $(CRT0) $(LIBUSER) user.ld
	@echo "Linking $@..."
	$(LD) $(LDFLAGS) -o $@ $(CRT0) $< $(LIBUSER)

%.o: %.c
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -c $< -o $@

$(LIBUSER): $(LIB_C_OBJS)
	@echo "Archiving $@..."
	$(AR) $(ARFLAGS) $@ $^

lib/%.o: lib/%.c
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -c $< -o $@

lib/crt0.o: lib/crt0.s
	@echo "Assembling $<..."
	$(AS_NASM) -f elf32 $< -o $@

clean:
	rm -f *.o *.elf lib/*.o $(LIBUSER)

install: all
	@echo "Installing C user programs to ../"
	@cp $(PROGRAMS) ..