#ifndef ELF_H
#define ELF_H

#include <stdint.h>

#define EI_NIDENT 16

/* Main ELF file header */
typedef struct {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;     /* Virtual address of the entry point (starting EIP) */
    uint32_t e_phoff;     /* Offset of the Program Header table in the file */
    uint32_t e_shoff;     /* Offset of the Section Header table */
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;     /* Number of Program Headers (segments) */
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf32_Ehdr;

/* Program/segment header (Program Header) */
typedef struct {
    uint32_t p_type;      /* Segment type (1 = PT_LOAD: loadable segment) */
    uint32_t p_offset;    /* Offset of the segment in the disk file */
    uint32_t p_vaddr;     /* Target virtual address in memory */
    uint32_t p_paddr;     /* Physical address (ignored) */
    uint32_t p_filesz;    /* Segment size on disk */
    uint32_t p_memsz;     /* Segment size in RAM (can be larger than p_filesz) */
    uint32_t p_flags;     /* Protection flags (1: X, 2: W, 4: R) */
    uint32_t p_align;     /* Alignment */
} Elf32_Phdr;

#define PT_LOAD 1  /* Constant indicating a loadable segment in memory */

#endif