/* arch/x86_64.c
 *
 * x86_64 architecture support.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "x86_64.h"
#include "../arch.h"
#include "../client.h"
#include "../defs.h"
#include "../log.h"
#include "../mem.h"
#include "../xutil.h"

static struct machine_specific x86_64_machine_specific;

static ulong *x86_64_kpgd_offset(ulong kvaddr)
{
    ulong *pgd;
    pgd = ((ulong *)machdep->pgd) + pgd_index(kvaddr);
    return pgd;
}

static ulong x86_64_pud_offset(ulong pgd_pte, ulong vaddr)
{
    ulong *pud;
    ulong pud_paddr;
    ulong pud_pte;

    pud_paddr = pgd_pte & PHYSICAL_PAGE_MASK;

    FILL_PUD(pud_paddr, PAGESIZE());
    pud = ((ulong *)pud_paddr) + pud_index(vaddr);
    pud_pte = ULONG(machdep->pud + PAGEOFFSET(pud));

    return pud_pte;
}

static ulong x86_64_pmd_offset(ulong pud_pte, ulong vaddr)
{
    ulong *pmd;
    ulong pmd_paddr;
    ulong pmd_pte;

    pmd_paddr = pud_pte & PHYSICAL_PAGE_MASK;

    FILL_PMD(pmd_paddr, PAGESIZE());

    pmd = ((ulong *)pmd_paddr) + pmd_index(vaddr);
    pmd_pte = ULONG(machdep->pmd + PAGEOFFSET(pmd));
    return pmd_pte;
}

static ulong x86_64_pte_offset(ulong pmd_pte, ulong vaddr)
{
    ulong *ptep;
    ulong pte_paddr;
    ulong pte;

    pte_paddr = pmd_pte & PHYSICAL_PAGE_MASK;

    FILL_PTBL(pte_paddr, PAGESIZE());
    ptep = ((ulong *)pte_paddr) + pte_index(vaddr);
    pte = ULONG(machdep->ptbl + PAGEOFFSET(ptep));

    return pte;
}

int x86_64_kvtop(ulong kvaddr, physaddr_t *paddr)
{
    ulong *pgd;
    ulong pud_pte;
    ulong pmd_pte;
    ulong pte;

    pgd = x86_64_kpgd_offset(kvaddr);
    pud_pte = x86_64_pud_offset(*pgd, kvaddr);
    pmd_pte = x86_64_pmd_offset(pud_pte, kvaddr);
    pte = x86_64_pte_offset(pmd_pte, kvaddr);
    *paddr = (PAGEBASE(pte) & PHYSICAL_PAGE_MASK) + PAGEOFFSET(kvaddr);

    return 0;
}

static ulong get_vec0_addr(ulong idtr)
{
    struct gate_struct64 {
        uint16_t offset_low;
        uint16_t segment;
        uint32_t ist : 3, zero0 : 5, type : 5, dpl : 2, p : 1;
        uint16_t offset_middle;
        uint32_t offset_high;
        uint32_t zero1;
    } __attribute__((packed)) gate;

    readmem(idtr, PHYSADDR, &gate, sizeof(gate));

    return ((ulong)gate.offset_high << 32)
        + ((ulong)gate.offset_middle << 16)
        + gate.offset_low;
}

#define PTI_USER_PGTABLE_BIT    PAGE_SHIFT
#define PTI_USER_PGTABLE_MASK   (1 << PTI_USER_PGTABLE_BIT)
#define CR3_PCID_MASK           0xFFFull

int x86_64_calc_kaslr_offset(ulong *kaslr_offset, ulong *phys_base)
{
    uint64_t cr3 = 0, idtr = 0, pgd = 0, idtr_paddr;
    ulong divide_error_vmcore;

    get_cr3_idtr(&cr3, &idtr);

    pgd = cr3 & ~(CR3_PCID_MASK|PTI_USER_PGTABLE_MASK);

    vt->kernel_pgd[0] = pgd;
    machdep->last_pgd_read = vt->kernel_pgd[0];
    machdep->machspec->physical_mask_shift = __PHYSICAL_MASK_SHIFT_2_6;
    machdep->machspec->pgdir_shift = PGDIR_SHIFT;
    machdep->machspec->ptrs_per_pgd = PTRS_PER_PGD;

    readmem(pgd, PHYSADDR, machdep->pgd, PAGESIZE());
    x86_64_kvtop(idtr, &idtr_paddr);

    divide_error_vmcore = get_vec0_addr(idtr_paddr);
    *kaslr_offset = divide_error_vmcore - st->divide_error_vmlinux;
    *phys_base = idtr_paddr -
        (st->idt_table_vmlinux + *kaslr_offset - __START_KERNEL_map);

    if (KDEBUG(1)) {
        pr_debug("kaslr_offset: idtr=%lx", idtr);
        pr_debug("kaslr_offset: pgd=%lx", pgd);
        pr_debug("kaslr_offset: idtr(phys)=%lx", idtr_paddr);
        pr_debug("kaslr_offset: divide_error(vmcore): %lx", divide_error_vmcore);
        pr_debug("kaslr_offset: kaslr_offset=%lx", *kaslr_offset);
        pr_debug("kaslr_offset: phys_base   =%lx", *phys_base);
    }

    return 0;
}

void x86_64_init(void)
{
    machdep->machspec = &x86_64_machine_specific;

    machdep->pagesize = 4096;
    machdep->pageoffset = machdep->pagesize - 1;
    machdep->pagemask = ~((ulonglong)machdep->pageoffset);

    machdep->pgd = malloc(PAGESIZE());
    machdep->pud = malloc(PAGESIZE());
    machdep->pmd = malloc(PAGESIZE());
    machdep->ptbl = malloc(PAGESIZE());

    machdep->machspec->page_offset = PAGE_OFFSET_2_6_27;
}

void x86_64_post_reloc(void)
{
    if (kernel_symbol_exists("page_offset_base")) {
        ulong page_offset_base = 0;
        get_symbol_data("page_offset_base", sizeof(ulong), &page_offset_base);
        if (page_offset_base)
            machdep->machspec->page_offset = page_offset_base;
    }
}

/* Public architecture interface */

int arch_init(void)
{
    x86_64_init();
    return 0;
}

int arch_calc_kaslr_offset(ulong *kaslr_offset, ulong *phys_base)
{
    if (x86_64_calc_kaslr_offset(kaslr_offset, phys_base) < 0) {
        pr_err("Failed to calculate KASLR offset");
        return -1;
    }
    return 0;
}

int arch_post_reloc(void)
{
    x86_64_post_reloc();
    return 0;
}

int arch_kvtop(ulong kvaddr, physaddr_t *paddr)
{
    /*
     * x86_64 kernel virtual addresses are either in the kernel image region
     * (__START_KERNEL_map, translated with phys_base) or in the direct linear
     * map (translated with PAGE_OFFSET).  This matches the behaviour of the
     * original kvm-dmesg and works for kernels that map the log buffer with
     * huge pages, which the page table walker does not handle.
     */
    if (kvaddr >= __START_KERNEL_map) {
        *paddr = (kvaddr - __START_KERNEL_map) + machdep->machspec->phys_base;
    } else {
        *paddr = kvaddr - PAGE_OFFSET;
    }
    return 0;
}
