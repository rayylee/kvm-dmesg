/* x86_64.c
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

#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>

#include "defs.h"
#include "client.h"

struct machine_specific x86_64_machine_specific = { 0 };

/*
 *  x86_64 __pa() clone.
 */
ulong x86_64_VTOP(ulong vaddr)
{
    if (vaddr >= __START_KERNEL_map)
        return ((vaddr) - (ulong)__START_KERNEL_map + machdep->machspec->phys_base);
    else
        return ((vaddr) - PAGE_OFFSET);
}

int x86_64_IS_VMALLOC_ADDR(ulong vaddr)
{
    return ((vaddr >= VMALLOC_START && vaddr <= VMALLOC_END) ||
            ((machdep->flags & VMEMMAP) && (vaddr >= VMEMMAP_VADDR && vaddr <= VMEMMAP_END)));
}

static ulong * x86_64_kpgd_offset(ulong kvaddr)
{
    ulong *pgd;
    pgd = ((ulong *)machdep->pgd) + pgd_index(kvaddr);
    return pgd;
}

ulong x86_64_pud_offset(ulong pgd_pte, ulong vaddr)
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

ulong x86_64_pmd_offset(ulong pud_pte, ulong vaddr)
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

ulong x86_64_pte_offset(ulong pmd_pte, ulong vaddr)
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

    if (!(machdep->flags & KSYMS_START)) {
        goto start_vtop_with_pagetable;
    }

    if (!IS_VMALLOC_ADDR(kvaddr)) {
        *paddr = x86_64_VTOP(kvaddr);
        return TRUE;
    }

start_vtop_with_pagetable:
    pgd = x86_64_kpgd_offset(kvaddr);
    pud_pte = x86_64_pud_offset(*pgd, kvaddr);
    pmd_pte = x86_64_pmd_offset(pud_pte, kvaddr);
    pte = x86_64_pte_offset(pmd_pte, kvaddr);
    *paddr = (PAGEBASE(pte) & PHYSICAL_PAGE_MASK) + PAGEOFFSET(kvaddr);

    return TRUE;
}

static uint memory_page_size(void)
{
    uint pagesz;

    if (machdep->pagesize)
        return machdep->pagesize;

    pagesz = (uint)getpagesize();
    return pagesz;
}

void x86_64_init()
{
    machdep->machspec = &x86_64_machine_specific;

    machdep->pagesize = memory_page_size();
    machdep->pageoffset = machdep->pagesize - 1;
    machdep->pagemask = ~((ulonglong)machdep->pageoffset);

    machdep->pgd = malloc(PAGESIZE());
    machdep->pud = malloc(PAGESIZE());
    machdep->pmd = malloc(PAGESIZE());
    machdep->ptbl = malloc(PAGESIZE());

    machdep->machspec->vmalloc_start_addr = VMALLOC_START_ADDR_2_6_11;
    machdep->machspec->vmalloc_end = VMALLOC_END_2_6_11;

    machdep->machspec->vmemmap_vaddr = VMEMMAP_VADDR_2_6_24;
    machdep->machspec->vmemmap_end = VMEMMAP_END_2_6_24;
    if (kernel_symbol_exists("vmemmap_populate"))
        machdep->flags |= VMEMMAP;

    machdep->machspec->page_offset = PAGE_OFFSET_2_6_27;

    machdep->kvtop = x86_64_kvtop;
}

void x86_64_post_reloc()
{
    machdep->flags |= KSYMS_START;

    if (kernel_symbol_exists("page_offset_base") &&
            kernel_symbol_exists("vmalloc_base")) {

        get_symbol_data("page_offset_base", sizeof(ulong),
            &machdep->machspec->page_offset);

        get_symbol_data("vmalloc_base", sizeof(ulong),
            &machdep->machspec->vmalloc_start_addr);
        machdep->machspec->vmalloc_end =
            machdep->machspec->vmalloc_start_addr + TERABYTES(32) - 1;

        if (kernel_symbol_exists("vmemmap_base")) {
            get_symbol_data("vmemmap_base", sizeof(ulong),
                    &machdep->machspec->vmemmap_vaddr);
            machdep->machspec->vmemmap_end =
                machdep->machspec->vmemmap_vaddr + TERABYTES(1) - 1;
        }
    }
}
