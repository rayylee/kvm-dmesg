/* arch/x86_64.h
 *
 * x86_64 specific declarations.
 */

#ifndef __ARCH_X86_64_H__
#define __ARCH_X86_64_H__

#include "../defs.h"

void x86_64_init(void);
void x86_64_post_reloc(void);
int x86_64_calc_kaslr_offset(ulong *kaslr_offset, ulong *phys_base);
int x86_64_kvtop(ulong kvaddr, physaddr_t *paddr);

#endif
