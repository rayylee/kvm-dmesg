/* arch.h
 *
 * Architecture abstraction layer.
 */

#ifndef __ARCH_H__
#define __ARCH_H__

#include "defs.h"

int arch_init(void);
int arch_calc_kaslr_offset(ulong *kaslr_offset, ulong *phys_base);
int arch_post_reloc(void);
int arch_kvtop(ulong kvaddr, physaddr_t *paddr);

#endif
