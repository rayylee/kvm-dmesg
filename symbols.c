/* symbols.c
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

#include "defs.h"
#include "log.h"
#include "client.h"

#define MAX_LINE_LENGTH 256

int symbol_needed(const char *symbol)
{
    static char *symtab_array[] = {
        "log_first_idx",
        "log_next_idx",
        "log_buf",
        "log_end",
        "log_buf_len",
        "divide_error",
        "asm_exc_divide_error",
        "idt_table",
        "vmcoreinfo_data",
        "vmcoreinfo_size",
        "page_offset_base",
        "vmalloc_base",
        "vmemmap_base",
        "vmemmap_populate",
        "prb"
    };
    size_t array_size = sizeof(symtab_array) / sizeof(symtab_array[0]);

    for (size_t i = 0; i < array_size; i++) {
        if (STREQ(symbol, symtab_array[i])) {
            return TRUE;
        }
    }
    return FALSE;
}

static void symname_hash_install(struct syment *spn)
{
    struct syment *sp;
    int index;

    index = SYMNAME_HASH_INDEX(spn->name);
    spn->cnt = 1;

    if ((sp = st->symname_hash[index]) == NULL)
        st->symname_hash[index] = spn;
    else {
        while (sp) {
            if (STREQ(sp->name, spn->name)) {
                sp->cnt++;
                spn->cnt++;
            }
            if (sp->name_hash_next)
                sp = sp->name_hash_next;
            else {
                sp->name_hash_next = spn;
                break;
            }
        }
    }
}

static struct syment *symname_hash_search(const char *name)
{
    struct syment *sp;

    sp = st->symname_hash[SYMNAME_HASH_INDEX(name)];

    while (sp) {
        if (STREQ(sp->name, name))
            return sp;
        sp = sp->name_hash_next;
    }

    return NULL;
}

static void symname_hash_init(const char *map_file)
{
   FILE *file = fopen(map_file, "r");
    if (file == NULL) {
        pr_err("Error opening file");
        return;
    }

    char line[MAX_LINE_LENGTH];
    while (fgets(line, sizeof(line), file)) {
        ulong address;
        char symbol[MAX_LINE_LENGTH];

        if (sscanf(line, "%lx %*s %s", &address, symbol) == 2) {
            if (symbol_needed(symbol)) {
                struct syment *sp = (struct syment *)calloc(1, sizeof(struct syment));
                sp->value = address;
                sp->name = strdup(symbol);
                symname_hash_install(sp);
            }

        }
    }
}

int kernel_symbol_exists(char *symbol)
{
    struct syment *sp;

    if ((sp = symname_hash_search(symbol)))
        return TRUE;
    else
        return FALSE;
}

struct syment *symbol_search(char *s)
{
    return symname_hash_search(s);
}

ulong symbol_value(char *symbol)
{
    struct syment *sp;

    if (!(sp = symbol_search(symbol))) {
        pr_err("cannot resolve symbol");
        return 0;
    }

    return (sp->value);
}

ulong relocate(ulong symval)
{
    if (kt->flags & RELOC_SET) {
        symval = symval - kt->relocate;
    }
    return symval;
}

void get_symbol_data(char *symbol, long size, void *local)
{
    struct syment *sp;

    if ((sp = symbol_search(symbol))) {
        readmem(relocate(sp->value), KVADDR, local, size);
    }
    else
        pr_err("cannot resolve symbol");
}

void symtab_init(const char *map_file)
{
    symname_hash_init(map_file);

    if (kernel_symbol_exists("asm_exc_divide_error")) {
        st->divide_error_vmlinux = symbol_value("asm_exc_divide_error");
    } else {
        st->divide_error_vmlinux = symbol_value("divide_error");
    }

    st->idt_table_vmlinux = symbol_value("idt_table");
}
