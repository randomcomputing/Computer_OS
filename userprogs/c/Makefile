# Makefile for Computer_OS 64-bit C user programs.

CC      = x86_64-elf-gcc
LD      = x86_64-elf-ld
AR      = x86_64-elf-ar
AS_NASM = nasm

CFLAGS = -ffreestanding -fno-asynchronous-unwind-tables -fno-pie \
         -fno-stack-protector -mno-red-zone -nostdlib -nostdinc \
         -mcmodel=small -mno-sse -mno-sse2 -mno-mmx \
         -Iinclude -O2 -Wall -Wextra

LDFLAGS = -T user.ld
ARFLAGS = rcs

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
	$(AS_NASM) -f elf64 $< -o $@

clean:
	rm -f *.o *.elf lib/*.o $(LIBUSER)

install: all
	@echo "Installing C user programs to ../"
	@cp $(PROGRAMS) ..