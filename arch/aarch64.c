/* arch/aarch64.c
 *
 * AArch64 architecture support.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aarch64.h"
#include "../arch.h"
#include "../defs.h"
#include "../log.h"
#include "../mem.h"
#include "../xutil.h"

static struct machine_specific aarch64_machine_specific;

void aarch64_init(void)
{
    machdep->machspec = &aarch64_machine_specific;

    machdep->pagesize = ARM64_PAGE_SIZE;
    machdep->pageoffset = machdep->pagesize - 1;
    machdep->pagemask = ~((ulonglong)machdep->pageoffset);

    machdep->pgd = malloc(PAGESIZE());
    machdep->pud = malloc(PAGESIZE());
    machdep->pmd = malloc(PAGESIZE());
    machdep->ptbl = malloc(PAGESIZE());

    machdep->machspec->page_offset = ARM64_PAGE_OFFSET;
    machdep->machspec->pgdir_shift = 0;
    machdep->machspec->ptrs_per_pgd = 0;
    machdep->machspec->physical_mask_shift = ARM64_VA_BITS;
}

void aarch64_post_reloc(void)
{
    /*
     * AArch64 uses kimage_voffset from vmcoreinfo for address translation.
     * Nothing else to adjust here.
     */
}

int aarch64_calc_kaslr_offset(ulong *kaslr_offset, ulong *phys_base)
{
    char vmcoreinfo_buf_local[4096];
    ulong kimage_voffset_value = 0;
    ulong kernel_offset = 0;
    ulong text_link;
    char *p;

    /*
     * QEMU does not expose TTBR1_EL1 through the human monitor command on
     * AArch64, so we cannot walk the page tables.  Instead scan guest RAM for
     * the vmcoreinfo note, which contains everything we need:
     *   NUMBER(kimage_voffset) : runtime_vaddr - phys_addr
     *   KERNELOFFSET           : KASLR randomization offset
     */
    if (mem_scan_vmcoreinfo(vmcoreinfo_buf_local, sizeof(vmcoreinfo_buf_local)) < 0) {
        pr_err("Failed to scan vmcoreinfo from guest RAM");
        return -1;
    }

    p = strstr(vmcoreinfo_buf_local, "NUMBER(kimage_voffset)=");
    if (p)
        sscanf(p + strlen("NUMBER(kimage_voffset)="), "%lx", &kimage_voffset_value);

    p = strstr(vmcoreinfo_buf_local, "KERNELOFFSET=");
    if (p)
        sscanf(p + strlen("KERNELOFFSET="), "%lx", &kernel_offset);

    if (!kimage_voffset_value) {
        pr_err("vmcoreinfo does not contain kimage_voffset");
        return -1;
    }

    /*
     * Keep the scanned vmcoreinfo so vmcoreinfo_init() does not have to read
     * vmcoreinfo_data, whose pointer may live in the vmalloc region and
     * therefore cannot be translated without a page table walk.
     */
    if (!vmcoreinfo_buf) {
        vmcoreinfo_size = sizeof(vmcoreinfo_buf_local) - 1;
        vmcoreinfo_buf = xmalloc(vmcoreinfo_size + 1);
        memcpy(vmcoreinfo_buf, vmcoreinfo_buf_local, vmcoreinfo_size);
        vmcoreinfo_buf[vmcoreinfo_size] = '\0';
    }

    if (kernel_symbol_exists("_text")) {
        text_link = symbol_value("_text");
    } else if (kernel_symbol_exists("_stext")) {
        text_link = symbol_value("_stext");
    } else {
        pr_err("_text/_stext symbol not found");
        return -1;
    }

    machdep->machspec->kimage_voffset = kimage_voffset_value;
    machdep->machspec->kaslr_offset = kernel_offset;

    /*
     * Runtime KIMAGE_VADDR is the link-time _text plus the KASLR offset.
     * Round it down to a 2 MB boundary so all kernel image addresses are
     * handled by the kimage_voffset path.
     */
    machdep->machspec->kimage_vaddr =
        (text_link + kernel_offset) & ~((1UL << 21) - 1);

    *kaslr_offset = kernel_offset;
    *phys_base = machdep->machspec->kimage_vaddr - kimage_voffset_value;

    if (KDEBUG(1)) {
        pr_debug("ARM64 kimage_voffset: 0x%lx", kimage_voffset_value);
        pr_debug("ARM64 KERNELOFFSET: 0x%lx", kernel_offset);
        pr_debug("ARM64 kimage_vaddr: 0x%lx", machdep->machspec->kimage_vaddr);
        pr_debug("ARM64 phys_base: 0x%lx", *phys_base);
    }

    return 0;
}

int aarch64_kvtop(ulong kvaddr, physaddr_t *paddr)
{
    if (machdep->machspec->kimage_vaddr &&
        kvaddr >= machdep->machspec->kimage_vaddr) {
        *paddr = kvaddr - machdep->machspec->kimage_voffset;
    } else if (kvaddr >= ARM64_PAGE_OFFSET) {
        *paddr = kvaddr - PAGE_OFFSET;
    } else if (machdep->machspec->kimage_voffset) {
        /* vmalloc / modules / etc */
        *paddr = kvaddr - machdep->machspec->kimage_voffset;
    } else {
        pr_err("Invalid ARM64 kernel address: 0x%lx", kvaddr);
        return -1;
    }

    return 0;
}

/* Public architecture interface */

int arch_init(void)
{
    aarch64_init();
    return 0;
}

int arch_calc_kaslr_offset(ulong *kaslr_offset, ulong *phys_base)
{
    if (aarch64_calc_kaslr_offset(kaslr_offset, phys_base) < 0) {
        pr_err("Failed to calculate KASLR offset");
        return -1;
    }
    return 0;
}

int arch_post_reloc(void)
{
    aarch64_post_reloc();
    return 0;
}

int arch_kvtop(ulong kvaddr, physaddr_t *paddr)
{
    return aarch64_kvtop(kvaddr, paddr);
}
