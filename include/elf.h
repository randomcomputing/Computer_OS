#ifndef ELF_H
#define ELF_H

typedef unsigned int   Elf32_Addr;
typedef unsigned int   Elf32_Off;
typedef unsigned short Elf32_Half;
typedef unsigned int   Elf32_Word;
typedef int            Elf32_Sword;

#define EI_NIDENT 16

typedef struct {
    unsigned char e_ident[EI_NIDENT];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
} Elf32_Ehdr;

typedef struct {
    Elf32_Word    p_type;
    Elf32_Off     p_offset;
    Elf32_Addr    p_vaddr;
    Elf32_Addr    p_paddr;
    Elf32_Word    p_filesz;
    Elf32_Word    p_memsz;
    Elf32_Word    p_flags;
    Elf32_Word    p_align;
} Elf32_Phdr;

// e_ident indices
#define EI_CLASS    4
#define EI_DATA     5
#define EI_VERSION  6

// EI_CLASS values
#define ELFCLASS32  1

// EI_DATA values
#define ELFDATA2LSB 1   // little-endian

// e_type
#define ET_EXEC     2

// e_machine
#define EM_386      3

// p_type
#define PT_LOAD     1

// p_flags
#define PF_X        0x1
#define PF_W        0x2
#define PF_R        0x4

static inline int elf32_valid(const unsigned char* buf, int n) {
    return n >= 4 &&
           buf[0] == 0x7f && buf[1] == 'E' &&
           buf[2] == 'L'  && buf[3] == 'F';
}

#endif
