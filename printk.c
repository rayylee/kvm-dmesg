/* printk.c
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
#include <ctype.h>

#include "xutil.h"
#include "client.h"
#include "log.h"
#include "defs.h"
#include "printk.h"

#define DESC_SV_BITS		(sizeof(unsigned long) * 8)
#define DESC_FLAGS_SHIFT	(DESC_SV_BITS - 2)
#define DESC_FLAGS_MASK		(3UL << DESC_FLAGS_SHIFT)
#define DESC_STATE(sv)		(3UL & (sv >> DESC_FLAGS_SHIFT))
#define DESC_ID_MASK		(~DESC_FLAGS_MASK)
#define DESC_ID(sv)	    	((sv) & DESC_ID_MASK)

enum desc_state {
    desc_miss	    =  -1,	/* ID mismatch (pseudo state) */
    desc_reserved	= 0x0,	/* reserved, in use by writer */
    desc_committed	= 0x1,	/* committed by writer, could get reopened */
    desc_finalized	= 0x2,	/* committed, no further modification allowed */
    desc_reusable	= 0x3,	/* free, not yet used by any writer */
};

static char *vmcoreinfo_buf = NULL;

char *vmcoreinfo_read_string(const char *key)
{
    const char *buf = vmcoreinfo_buf;
    char *value_string = NULL;
    char *p1, *p2;
    size_t value_length;
    char keybuf[64] = {0};

    sprintf(keybuf, "%s=", key);

    if ((p1 = strstr(buf, keybuf))) {
        p2 = p1 + strlen(keybuf);
        p1 = strstr(p2, "\n");
        value_length = p1 - p2;
        value_string = xcalloc(value_length + 1, sizeof(char));
        strncpy(value_string, p2, value_length);
        value_string[value_length] = '\0';
    }

    return value_string;
}

long datatype_info(char *name, char *member, int datatype)
{
    char buf[64] = {0};
    char *value_string;
    char *endptr;
    long value  = 0;

    if (datatype == STRUCT_SIZE_REQUEST){
        sprintf(buf, "SIZE(%s)", name);
    } else if (datatype == MEMBER_OFFSET_REQUEST) {
        sprintf(buf, "OFFSET(%s.%s)", name, member);
    }

    if (strlen(buf)) {
        value_string = vmcoreinfo_read_string(buf);
        if (value_string) {
            value = strtol(value_string, &endptr, 10);
            xfree(value_string);
        }
   }

    return value;
}

void vmcoreinfo_init()
{
    char *buf;
    size_t vmcoreinfo_size;
    ulong vmcoreinfo_data;

    get_symbol_data("vmcoreinfo_size", sizeof(vmcoreinfo_size), &vmcoreinfo_size);
    vmcoreinfo_size &= ((1<<13) - 1);

    vmcoreinfo_buf = xmalloc(vmcoreinfo_size + 1);
    buf = vmcoreinfo_buf;
    get_symbol_data("vmcoreinfo_data", sizeof(vmcoreinfo_data), &vmcoreinfo_data);
    if (readmem(vmcoreinfo_data, KVADDR, buf, vmcoreinfo_size)) {
        pr_err("cannot read vmcoreinfo_data\n");
        goto err;
    }
    buf[vmcoreinfo_size] = '\n';

    if (CRASHDEBUG(2)) {
        for (size_t i = 0; i < vmcoreinfo_size; i++) {
            fprintf(fp, "%c", buf[i]);
        }
        fprintf(fp, "\n");
    }
    return;
err:
    xfree(vmcoreinfo_buf);
}

static void offsets_init()
{
    char *n;

    n = "printk_info";
    STRUCT_SIZE_INIT(printk_info, n);

    n = "prb_desc";
    STRUCT_SIZE_INIT(prb_desc, n);

    n = "printk_ringbuffer";
    STRUCT_SIZE_INIT(printk_ringbuffer, n);
    MEMBER_OFFSET_INIT(prb_desc_ring, n, "desc_ring");
    MEMBER_OFFSET_INIT(prb_text_data_ring, n, "text_data_ring");

    n = "prb_desc_ring";
    STRUCT_SIZE_INIT(prb_desc_ring, n);
    MEMBER_OFFSET_INIT(prb_desc_ring_count_bits, n, "count_bits");
    MEMBER_OFFSET_INIT(prb_desc_ring_descs, n, "descs");
    MEMBER_OFFSET_INIT(prb_desc_ring_infos, n, "infos");
    MEMBER_OFFSET_INIT(prb_desc_ring_head_id, n, "head_id");
    MEMBER_OFFSET_INIT(prb_desc_ring_tail_id, n, "tail_id");

    n = "prb_data_ring";
    STRUCT_SIZE_INIT(prb_data_ring, n);
    MEMBER_OFFSET_INIT(prb_data_ring_size_bits, n, "size_bits");
    MEMBER_OFFSET_INIT(prb_data_ring_data, n, "data");
}

static enum desc_state get_desc_state(unsigned long id,
        unsigned long state_val)
{
    if (id != DESC_ID(state_val))
        return desc_miss;

    return DESC_STATE(state_val);
}

static void dump_record(struct prb_map *m, unsigned long id)
{
    unsigned short text_len;
    unsigned long state_var;
    char *desc, *info, *text, *p;
    enum desc_state state;
    unsigned long begin;
    unsigned long next;
    uint64_t ts_nsec;
    unsigned long long nanos;
    unsigned long rem;
    char buf[1024];
    int i;

    desc = m->descs + ((id % m->desc_ring_count) * sizeof(struct prb_desc));

    state_var = ULONG(desc + offsetof(struct prb_desc, state_var) +
            offsetof(atomic_long_t, counter));
    state = get_desc_state(id, state_var);

    if (state != desc_committed && state != desc_finalized)
        return;

    info = m->infos + ((id % m->desc_ring_count) * sizeof(struct printk_info));

    text_len = USHORT(info + offsetof(struct printk_info, text_len));

    begin = ULONG(desc + offsetof(struct prb_desc, text_blk_lpos) +
            offsetof(struct prb_data_blk_lpos, begin)) % m->text_data_ring_size;
    next = ULONG(desc + offsetof(struct prb_desc, text_blk_lpos) +
            offsetof(struct prb_data_blk_lpos, next)) % m->text_data_ring_size;

    if (begin == next)
        goto out;

    ts_nsec = ULONGLONG(info + offsetof(struct printk_info, ts_nsec));
    nanos = (unsigned long long)ts_nsec / (unsigned long long)1000000000;
    rem = (unsigned long long)ts_nsec % (unsigned long long)1000000000;
    sprintf(buf, "[%5lld.%06ld] ", nanos, rem/1000);
    fprintf(fp, "%s", buf);

    if (begin > next)
        begin = 0;

    begin += sizeof(unsigned long);

    if (next - begin < text_len)
        text_len = next - begin;

    text = m->text_data + begin;

    for (i = 0, p = text; i < text_len; i++, p++) {
        if (*p == '\n')
            fprintf(fp, "\n");
        else if (isprint(*p) || isspace(*p))
            fputc(*p, fp);
        else
            fputc('.', fp);
    }

out:
    fprintf(fp, "\n");
}

void dump_lockless_record_log()
{
    unsigned long head_id;
    unsigned long tail_id;
    unsigned long kaddr;
    unsigned long id;
    struct prb_map m;

    if (SIZE(printk_info) == 0) {
        offsets_init();
    }

    get_symbol_data("prb", sizeof(char *), &kaddr);
    m.prb = xmalloc(SIZE(printk_ringbuffer));

    if (readmem(kaddr, KVADDR, m.prb, SIZE(printk_ringbuffer))) {
        pr_err("Cannot read printk_ringbuffer contents");
        goto out_prb;
    }

    m.desc_ring = m.prb + OFFSET(prb_desc_ring);
    m.desc_ring_count = 1 << UINT(m.desc_ring + OFFSET(prb_desc_ring_count_bits));

    kaddr = ULONG(m.desc_ring + OFFSET(prb_desc_ring_descs));
    m.descs = xmalloc(SIZE(prb_desc) * m.desc_ring_count);

    if (readmem(kaddr, KVADDR, m.descs, SIZE(prb_desc) * m.desc_ring_count)) {
        pr_err("Cannot read prb_desc_ring contents");
        goto out_descs;
    }

    kaddr = ULONG(m.desc_ring + OFFSET(prb_desc_ring_infos));
    m.infos = xmalloc(SIZE(printk_info) * m.desc_ring_count);

    if (readmem(kaddr, KVADDR, m.infos, SIZE(printk_info) * m.desc_ring_count)) {
        pr_err("Cannot read prb_info_ring contents");
        goto out_infos;
    }

    m.text_data_ring = m.prb + OFFSET(prb_text_data_ring);
    m.text_data_ring_size = 1 << UINT(m.text_data_ring + OFFSET(prb_data_ring_size_bits));

    kaddr = ULONG(m.text_data_ring + OFFSET(prb_data_ring_data));
    m.text_data = xmalloc(m.text_data_ring_size);

    if (readmem(kaddr, KVADDR, m.text_data, m.text_data_ring_size)) {
        pr_err("Cannot read prb_text_data_ring contents");
        goto out_text_data;
    }

    tail_id = ULONG(m.desc_ring + OFFSET(prb_desc_ring_tail_id) +
            offsetof(atomic_long_t, counter));
    head_id = ULONG(m.desc_ring + OFFSET(prb_desc_ring_head_id) +
            offsetof(atomic_long_t, counter));

    for (id = tail_id; id != head_id; id = (id + 1) & DESC_ID_MASK) {
        dump_record(&m, id);
    }

    dump_record(&m, id);

out_text_data:
    xfree(m.text_data);
out_infos:
    xfree(m.infos);
out_descs:
    xfree(m.descs);
out_prb:
    xfree(m.prb);
}
