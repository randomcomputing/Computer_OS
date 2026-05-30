#ifndef ELF_H
#define ELF_H

#include "stdint.h"

#define EI_NIDENT 16

#define EI_CLASS    4
#define EI_DATA     5
#define EI_VERSION  6

#define ELFCLASS32  1
#define ELFCLASS64  2

#define ELFDATA2LSB 1

#define ET_EXEC     2

#define EM_386      3
#define EM_X86_64   62

#define PT_LOAD     1

#define PF_X        0x1
#define PF_W        0x2
#define PF_R        0x4

typedef uint16_t Elf64_Half;
typedef uint32_t Elf64_Word;
typedef int32_t  Elf64_Sword;
typedef uint64_t Elf64_Addr;
typedef uint64_t Elf64_Off;
typedef uint64_t Elf64_Xword;
typedef int64_t  Elf64_Sxword;

typedef struct {
    unsigned char e_ident[EI_NIDENT];
    Elf64_Half    e_type;
    Elf64_Half    e_machine;
    Elf64_Word    e_version;
    Elf64_Addr    e_entry;
    Elf64_Off     e_phoff;
    Elf64_Off     e_shoff;
    Elf64_Word    e_flags;
    Elf64_Half    e_ehsize;
    Elf64_Half    e_phentsize;
    Elf64_Half    e_phnum;
    Elf64_Half    e_shentsize;
    Elf64_Half    e_shnum;
    Elf64_Half    e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    Elf64_Word  p_type;
    Elf64_Word  p_flags;
    Elf64_Off   p_offset;
    Elf64_Addr  p_vaddr;
    Elf64_Addr  p_paddr;
    Elf64_Xword p_filesz;
    Elf64_Xword p_memsz;
    Elf64_Xword p_align;
} Elf64_Phdr;

static inline int elf_valid_magic(const unsigned char* buf, int n) {
    return n >= 4 &&
           buf[0] == 0x7f &&
           buf[1] == 'E' &&
           buf[2] == 'L' &&
           buf[3] == 'F';
}

static inline int elf64_valid(const unsigned char* buf, int n) {
    if (!elf_valid_magic(buf, n)) return 0;
    if (n < (int)sizeof(Elf64_Ehdr)) return 0;

    const Elf64_Ehdr* eh = (const Elf64_Ehdr*)buf;

    return eh->e_ident[EI_CLASS] == ELFCLASS64 &&
           eh->e_ident[EI_DATA] == ELFDATA2LSB &&
           eh->e_type == ET_EXEC &&
           eh->e_machine == EM_X86_64;
}

#endif