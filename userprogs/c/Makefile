# Makefile for Computer_OS C user programs.
#
# Each .c at the top level becomes an ELF32 executable that the kernel's
# ELF loader reads from the FAT12 disk.  Shared startup (crt0.s) and
# library code (libuser.a) are built from sources in lib/.
#
# Targets:
#   make             -> build every program: testc.elf, echo.elf, ...
#   make echo.elf    -> build just one
#   make clean       -> remove all .o, .a, .elf
#   make install     -> copy *.elf into the parent userprogs/ dir
#
# Use the same i686-elf cross-compiler the kernel uses. If you don't
# have it installed but do have host gcc with -m32, the host-build
# fallback below works (it's what I use for CI).

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
LIB_C_SRCS  := lib/stdio.c lib/string.c
LIB_S_SRCS  := lib/crt0.s

LIB_C_OBJS  := $(LIB_C_SRCS:.c=.o)
LIB_S_OBJS  := $(LIB_S_SRCS:.s=.o)

# crt0 is NOT in libuser.a — the linker needs to place it first via
# the .text.start section, and putting it inside an archive lets ld
# decide whether to pull it in (it might not). So we link it as a
# standalone object.
CRT0 = lib/crt0.o

# All other library objects are bundled into an .a so user programs
# can just say `-luser`.
LIBUSER = libuser.a

# Find every top-level .c that isn't lib/* and turn it into an .elf.
PROGRAMS_C := $(wildcard *.c)
PROGRAMS   := $(PROGRAMS_C:.c=.elf)

# ---- top-level rules ------------------------------------------------

.PHONY: all clean install
all: $(PROGRAMS)

# How an .elf is built: compile the C source, link with crt0 + libuser
# against user.ld to produce an ELF32 executable.
%.elf: %.o $(CRT0) $(LIBUSER) user.ld
	@echo "Linking $@..."
	$(LD) $(LDFLAGS) -o $@ $(CRT0) $< $(LIBUSER)

%.o: %.c
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -c $< -o $@

# ---- libuser.a + crt0 ----------------------------------------------

$(LIBUSER): $(LIB_C_OBJS)
	@echo "Archiving $@..."
	$(AR) $(ARFLAGS) $@ $^

lib/%.o: lib/%.c
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -c $< -o $@

lib/crt0.o: lib/crt0.s
	@echo "Assembling $<..."
	$(AS_NASM) -f elf32 $< -o $@

# ---- maintenance ----------------------------------------------------

clean:
	rm -f *.o *.elf lib/*.o $(LIBUSER)

install: all
	@echo "Installing C user programs to ../"
	@cp $(PROGRAMS) ..