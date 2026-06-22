/* arch/aarch64.h
 *
 * AArch64 specific declarations.
 */

#ifndef __ARCH_AARCH64_H__
#define __ARCH_AARCH64_H__

#include "../defs.h"

#define ARM64_VA_BITS          48
#define ARM64_PAGE_SHIFT       12
#define ARM64_PAGE_SIZE        (1UL << ARM64_PAGE_SHIFT)
#define ARM64_PAGE_OFFSET      0xffff800000000000UL

void aarch64_init(void);
void aarch64_post_reloc(void);
int aarch64_calc_kaslr_offset(ulong *kaslr_offset, ulong *phys_base);
int aarch64_kvtop(ulong kvaddr, physaddr_t *paddr);

#endif
