/* defs.h
 *
 * Copyright (C) 2024 Ray Lee
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef __DEFS_H__
#define __DEFS_H__

#include <sys/types.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>


#undef TRUE
#undef FALSE

#define TRUE  (1)
#define FALSE (0)

#define NR_CPUS  (256)

static inline int string_exists(char *s) { return (s ? TRUE : FALSE); }
#define STREQ(A, B) (string_exists((char *)A) && string_exists((char *)B) && \
        (strcmp((char *)(A), (char *)(B)) == 0))


#ifdef roundup
#undef roundup
#endif
#define roundup(x, y)  ((((x)+((y)-1))/(y))*(y))

typedef uint64_t physaddr_t;

typedef unsigned long long int ulonglong;

#define KDEBUG(x) (pc->debug >= (x))

struct program_context {
    ulong debug;                    /* level of debug */
};

#define RELOC_SET            (0x2000000)

struct kernel_table {
    ulong flags;
    ulong relocate;
	uint kernel_version[3];
};

struct machdep_table {
	ulong flags;
	int (*kvtop)(ulong, physaddr_t *);

	char *pgd;
	char *pud;
	char *pmd;
	char *ptbl;

	ulong last_pgd_read;
	ulong last_pud_read;
	ulong last_pmd_read;
	ulong last_ptbl_read;

	struct machine_specific *machspec;
	unsigned int pagesize;
	unsigned long pageoffset;
	ulonglong pagemask;
};

#define VMEMMAP           (0x400000)

extern struct machdep_table *machdep;

#define NULLCHAR ('\0')

#define IS_LAST_PGD_READ(pgd)     ((ulong)(pgd) == machdep->last_pgd_read)
#define IS_LAST_PMD_READ(pmd)     ((ulong)(pmd) == machdep->last_pmd_read)
#define IS_LAST_PTBL_READ(ptbl)   ((ulong)(ptbl) == machdep->last_ptbl_read)
#define IS_LAST_PUD_READ(pud)     ((ulong)(pud) == machdep->last_pud_read)


#define FILL_PGD(PGD, SIZE)                       \
    if (!IS_LAST_PGD_READ(PGD)) {                                             \
            readmem((ulonglong)((ulong)(PGD)), PHYSADDR, machdep->pgd, SIZE); \
            machdep->last_pgd_read = (ulong)(PGD);                            \
    }

#define FILL_PUD(PUD, SIZE)                       \
    if (!IS_LAST_PUD_READ(PUD)) {                                             \
            readmem((ulonglong)((ulong)(PUD)), PHYSADDR, machdep->pud, SIZE); \
            machdep->last_pud_read = (ulong)(PUD);                            \
    }

#define FILL_PMD(PMD, SIZE)                               \
    if (!IS_LAST_PMD_READ(PMD)) {                                             \
            readmem((ulonglong)(PMD), PHYSADDR, machdep->pmd, SIZE);          \
            machdep->last_pmd_read = (ulong)(PMD);                            \
    }

#define FILL_PTBL(PTBL, SIZE)                         \
    if (!IS_LAST_PTBL_READ(PTBL)) {                                           \
            readmem((ulonglong)(PTBL), PHYSADDR, machdep->ptbl, SIZE);        \
            machdep->last_ptbl_read = (ulong)(PTBL);                          \
    }

struct offset_table {
    // struct printk_ringbuffer
    long prb_desc_ring;
    long prb_text_data_ring;

    // struct prb_desc_ring
    long prb_desc_ring_count_bits;
    long prb_desc_ring_descs;
    long prb_desc_ring_infos;
    long prb_desc_ring_head_id;
    long prb_desc_ring_tail_id;

    // struct prb_data_ring
    long prb_data_ring_size_bits;
    long prb_data_ring_data;
};

struct size_table {
    long printk_info;
    long prb_desc;

    long printk_ringbuffer;
    long prb_desc_ring;
    long prb_data_ring;
};

#define MEMBER_OFFSET_REQUEST (0)
#define STRUCT_SIZE_REQUEST   (-4)

#define STRUCT_SIZE(X)      datatype_info((X), NULL, STRUCT_SIZE_REQUEST)
#define MEMBER_OFFSET(X,Y)  datatype_info((X), (Y), MEMBER_OFFSET_REQUEST)

#define OFFSET(X)          (offset_table.X)
#define SIZE(X)            (size_table.X)
#define ASSIGN_SIZE(X)     (size_table.X)
#define ASSIGN_OFFSET(X)   (offset_table.X)

#define STRUCT_SIZE_INIT(X, Y) (ASSIGN_SIZE(X) = STRUCT_SIZE(Y))
#define MEMBER_OFFSET_INIT(X, Y, Z) (ASSIGN_OFFSET(X) = MEMBER_OFFSET(Y, Z))

#define ULONG(ADDR)     *((ulong *)((char *)(ADDR)))
#define UINT(ADDR)      *((uint *)((char *)(ADDR)))
#define USHORT(ADDR)    *((ushort *)((char *)(ADDR)))
#define ULONGLONG(ADDR) *((ulonglong *)((char *)(ADDR)))

struct vm_table {
    ulong kernel_pgd[NR_CPUS];
};

struct syment {
    ulong value;
    char *name;
    struct syment *name_hash_next;
    unsigned char cnt;
};

#define SYMNAME_HASH (512)
#define SYMNAME_HASH_INDEX(name) \
     ((name[0] ^ (name[strlen(name)-1] * name[strlen(name)/2])) % SYMNAME_HASH)

struct symbol_table_data {
    struct syment *symname_hash[SYMNAME_HASH];
    ulong divide_error_vmlinux;
    ulong idt_table_vmlinux;
};

#define KVADDR             (0x1)
#define UVADDR             (0x2)
#define PHYSADDR           (0x4)

#define PAGESIZE()    (machdep->pagesize)

#define PAGEOFFSET(X) (((ulong)(X)) & machdep->pageoffset)

#define PAGEBASE(X)     (((ulong)(X)) & (ulong)machdep->pagemask)

/*
 * x86_64.c
 */
void x86_64_init();
void x86_64_post_reloc();
int x86_64_kvtop(ulong kvaddr, physaddr_t *paddr);

struct machine_specific {
    ulong page_offset;
    ulong vmalloc_start_addr;
    ulong vmalloc_end;
    ulong vmemmap_vaddr;
    ulong vmemmap_end;
    ulong phys_base;
    ulong pgdir_shift;
    ulong ptrs_per_pgd;
    ulong physical_mask_shift;
};

#define KSYMS_START    (0x1)

#define PAGE_OFFSET     (machdep->machspec->page_offset)
#define VMALLOC_START   (machdep->machspec->vmalloc_start_addr)
#define VMALLOC_END     (machdep->machspec->vmalloc_end)
#define VMEMMAP_VADDR   (machdep->machspec->vmemmap_vaddr)
#define VMEMMAP_END     (machdep->machspec->vmemmap_end)

#define __START_KERNEL_map    0xffffffff80000000UL

#define VMALLOC_START_ADDR_2_6_11  0xffffc20000000000
#define VMALLOC_END_2_6_11         0xffffe1ffffffffff

#define VMEMMAP_VADDR_2_6_24       0xffffe20000000000
#define VMEMMAP_END_2_6_24         0xffffe2ffffffffff

#define PAGE_OFFSET_2_6_27         0xffff880000000000

#define IS_VMALLOC_ADDR(X)    x86_64_IS_VMALLOC_ADDR((ulong)(X))

/*
 * the default page table level for x86_64:
 *    4 level page tables
 */
#define PGDIR_SHIFT     39
#define PTRS_PER_PGD    512
#define PUD_SHIFT       30
#define PTRS_PER_PUD    512
#define PMD_SHIFT       21
#define PTRS_PER_PMD    512
#define PTRS_PER_PTE    512


#define __PGDIR_SHIFT  (machdep->machspec->pgdir_shift)
#define __PTRS_PER_PGD  (machdep->machspec->ptrs_per_pgd)

#define pgd_index(address)  (((address) >> __PGDIR_SHIFT) & (__PTRS_PER_PGD-1))


#define pud_index(address)  (((address) >> PUD_SHIFT) & (PTRS_PER_PUD - 1))
#define pmd_index(address)  (((address) >> PMD_SHIFT) & (PTRS_PER_PMD-1))
#define pte_index(address)  (((address) >> PAGE_SHIFT) & (PTRS_PER_PTE - 1))

#define __PHYSICAL_MASK_SHIFT_2_6     46

#define __PHYSICAL_MASK_SHIFT  (machdep->machspec->physical_mask_shift)
#define __PHYSICAL_MASK        ((1UL << __PHYSICAL_MASK_SHIFT) - 1)

#define PAGE_SHIFT             12
#define PAGE_SIZE              (1UL << PAGE_SHIFT)
#define PHYSICAL_PAGE_MASK    (~(PAGE_SIZE-1) & __PHYSICAL_MASK )

#define KILOBYTES(x)  ((x) * (1024))
#define MEGABYTES(x)  ((x) * (1048576))
#define GIGABYTES(x)  ((x) * (1073741824))
#define TB_SHIFT (40)
#define TERABYTES(x) ((x) * (1UL << TB_SHIFT))

#define MEGABYTE_MASK (MEGABYTES(1)-1)

#define SIZEOF_64BIT  (8)
#define SIZEOF_32BIT  (4)
#define SIZEOF_16BIT  (2)

/*
 *  Global data (global_data.c)
 */
extern FILE *fp;
extern struct program_context program_context, *pc;
extern struct kernel_table kernel_table, *kt;
extern struct offset_table offset_table;
extern struct size_table size_table;
extern struct vm_table vm_table, *vt;
extern struct symbol_table_data symbol_table_data, *st;


/*
 * symbols.c
 */
void symtab_init(const char*);
ulong symbol_value(char *);
int kernel_symbol_exists(char *s);


/*
 *  symbols.c
 */
void get_symbol_data(char *symbol, long size, void *local);

void kernel_init(void);
long datatype_info(char *name, char *member, int datatype);
void parse_kernel_version(char *);
void vmcoreinfo_init();
void die(const char *err, ...);
char *vmcoreinfo_read_string(const char *key);
#endif
