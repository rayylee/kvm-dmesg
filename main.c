/* main.c
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

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdint.h>
#include <string.h>
#include <fnmatch.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include "log.h"
#include "defs.h"
#include "client.h"
#include "version.h"
#include "printk.h"

struct machine_specific x86_64_machine_specific = { 0 };

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

    pgd = x86_64_kpgd_offset(kvaddr);
    pud_pte = x86_64_pud_offset(*pgd, kvaddr);
    pmd_pte = x86_64_pmd_offset(pud_pte, kvaddr);
    pte = x86_64_pte_offset(pmd_pte, kvaddr);
    *paddr = (PAGEBASE(pte) & PHYSICAL_PAGE_MASK) + PAGEOFFSET(kvaddr);

    return 0;
}

ulong get_vec0_addr(ulong idtr)
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

void write_data_to_file(const char *filename, void *data, size_t size) {
    FILE *file = fopen(filename, "wb");
    if (file == NULL) {
        pr_err("Failed to open file");
        return;
    }

    size_t written = fwrite(data, 1, size, file);
    if (written != size) {
        pr_err("Failed to write data to file");
    }

    fclose(file);
}

#define PTI_USER_PGTABLE_BIT    PAGE_SHIFT
#define PTI_USER_PGTABLE_MASK   (1 << PTI_USER_PGTABLE_BIT)
#define CR3_PCID_MASK           0xFFFull
int calc_kaslr_offset(ulong *kaslr_offset, ulong *phys_base)
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

    if (CRASHDEBUG(1)) {
        pr_debug("kaslr_offset: idtr=%lx", idtr);
        pr_debug("kaslr_offset: pgd=%lx", pgd);
        pr_debug("kaslr_offset: idtr(phys)=%lx", idtr_paddr);
        pr_debug("kaslr_offset: divide_error(vmcore): %lx", divide_error_vmcore);
    }

    if (CRASHDEBUG(1)) {
        pr_debug("kaslr_offset: kaslr_offset=%lx", *kaslr_offset);
        pr_debug("kaslr_offset: phys_base   =%lx", *phys_base);
    }

    return 0;
}

void x86_64_init()
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

void x86_64_post_reloc()
{
    if (kernel_symbol_exists("page_offset_base")) {
        get_symbol_data("page_offset_base", sizeof(ulong),
            &machdep->machspec->page_offset);
    }
}

void derive_kaslr_offset()
{
    ulong kaslr_offset = 0;
    ulong phys_base = 0;

    calc_kaslr_offset(&kaslr_offset, &phys_base);

    if (kaslr_offset) {
        kt->relocate = kaslr_offset * -1;
        kt->flags |= RELOC_SET;
    }

    machdep->machspec->phys_base = phys_base;
}

int ascii(int c)
{
    return ((c >= 0) && ( c <= 0x7f));
}

static char* log_from_idx(uint32_t idx, char *logbuf)
{
    char *logptr;
    uint16_t msglen;

    logptr = logbuf + idx;

    msglen = USHORT(logptr + offsetof(struct log, len));
    if (!msglen)
        logptr = logbuf;

    return logptr;
}

static uint32_t log_next(uint32_t idx, char *logbuf)
{
    char *logptr;
    uint16_t msglen;

    logptr = logbuf + idx;

    msglen = USHORT(logptr + offsetof(struct log, len));
    if (!msglen) {
        msglen = USHORT(logbuf + offsetof(struct log, len));
        return msglen;
    }

    return idx + msglen;
}

static void dump_log_entry(char *logptr)
{
    char *msg, *p;
    uint16_t i, text_len;
    uint64_t ts_nsec;
    ulonglong nanos;
    ulong rem;
    char buf[64];

    text_len = USHORT(logptr + offsetof(struct log, text_len));

    ts_nsec = ULONGLONG(logptr);
    msg = logptr + sizeof(struct log);

    nanos = (ulonglong)ts_nsec / (ulonglong)1000000000;
    rem = (ulonglong)ts_nsec % (ulonglong)1000000000;
    sprintf(buf, "[%5lld.%06ld] ", nanos, rem/1000);
    fprintf(fp, "%s", buf);

    for (i = 0, p = msg; i < text_len; i++, p++) {
        if (*p == '\n')
            fprintf(fp, "\n");
        else if (isprint(*p) || isspace(*p))
            fputc(*p, fp);
        else
            fputc('.', fp);
    }

    fprintf(fp, "\n");
}

static void dump_variable_length_record_log(void)
{
    uint32_t idx, log_first_idx, log_next_idx, log_buf_len;
    ulong log_buf;
    char *logptr, *logbuf;

    get_symbol_data("log_first_idx", sizeof(uint32_t), &log_first_idx);
    get_symbol_data("log_next_idx", sizeof(uint32_t), &log_next_idx);
    get_symbol_data("log_buf_len", sizeof(uint32_t), &log_buf_len);
    get_symbol_data("log_buf", sizeof(char *), &log_buf);

    if (CRASHDEBUG(1)) {
        pr_debug("log_buf: %lx", (ulong)log_buf);
        pr_debug("log_buf_len: %d", log_buf_len);
        pr_debug("log_first_idx: %d", log_first_idx);
        pr_debug("log_next_idx: %d", log_next_idx);
    }

    log_buf_len &= ((1<<20) | ((1<<20) - 1));
    logbuf = (char *)malloc(log_buf_len);

    readmem(log_buf, KVADDR, logbuf, log_buf_len);

    idx = log_first_idx;
    while (idx != log_next_idx) {
        logptr = log_from_idx(idx, logbuf);
        dump_log_entry(logptr);

        idx = log_next(idx, logbuf);

        if (idx >= log_buf_len) {
            break;
        }
    }
}

static int is_text_file(const char *path)
{
    unsigned char byte;
    size_t bytes_read = 0;
    FILE *file = fopen(path, "rb");

    if (file == NULL) {
        pr_err("Failed to open file: %s", path);
        return -1;
    }

    while (bytes_read < 1024  && fread(&byte, 1, 1, file) == 1) {
        if (!isprint(byte) && !isspace(byte) && byte != '\0') {
            fclose(file);
            return 0;
        }
        bytes_read++;
    }

    fclose(file);
    return 1;
}

int main(int argc, char *argv[])
{
    struct stat path_stat;
    char *symmap_file = NULL;
    char *guest_ac = NULL;
    guest_access_t ac_type;

    pc->debug = 1;
    fp = stdout;

    fprintf(fp, "Version %s\n\n", get_version_text());

    if (argc != 3 ) {
        fprintf(fp, "Usage: %s <domain_name/socket_path> <system.map>\n", argv[0]);
        return -1;
    }

    if (!stat(argv[1], &path_stat) && S_ISREG(path_stat.st_mode)) {
        if ( is_text_file(argv[1]) == 1) {
            symmap_file = argv[1];
            guest_ac = argv[2];
        }
    }

    if (!symmap_file) {
        if (!stat(argv[2], &path_stat) && S_ISREG(path_stat.st_mode)) {
            if ( is_text_file(argv[2]) == 1) {
                symmap_file = argv[2];
                guest_ac = argv[1];
            }
        }
    }

    if (!symmap_file) {
        pr_err("System.map file not foound");
        return -1;
    }

    if (stat(guest_ac, &path_stat) == 0) {
        if (S_ISREG(path_stat.st_mode)) {
            ac_type = GUEST_MEMORY;
        } else if (S_ISSOCK(path_stat.st_mode)) {
            ac_type = QMP_SOCKET;
        } else {
            pr_err("Unknown file type: %s", guest_ac);
            return -1;
        }
    } else {
        ac_type = GUEST_NAME;
    }

    fprintf(fp, "Guest: %s\n", guest_ac);
    fprintf(fp, "System.map: %s\n", symmap_file);

    guest_client_new(guest_ac, ac_type);

    symtab_init(symmap_file);

    x86_64_init();

    derive_kaslr_offset();

    x86_64_post_reloc();

    vmcoreinfo_init();
    kernel_init();

    if (kernel_symbol_exists("prb")) {
        dump_lockless_record_log();
        goto exit;
    }

    if (kernel_symbol_exists("log_first_idx") &&
            kernel_symbol_exists("log_next_idx")) {
        dump_variable_length_record_log();
        goto exit;
    }

    ulong log_buf_len = 0;
    ulong log_buf = 0;

    get_symbol_data("log_buf", sizeof(char *), &log_buf);
    get_symbol_data("log_buf_len", sizeof(uint32_t), &log_buf_len);

    log_buf_len &= ((1<<20) | ((1<<20) - 1));
    char *logbuf_arry = malloc(log_buf_len);

    if (CRASHDEBUG(1)) {
        pr_debug("log_buf len: %ld (0x%lx)", log_buf_len, log_buf_len);
        pr_debug("log_buf addr: 0x%lx", log_buf);
    }
    readmem(log_buf, KVADDR, logbuf_arry, log_buf_len);

    int next_line = FALSE;
    for (ulong i = 0; i < log_buf_len; i++) {
        if (logbuf_arry[i]) {
            if (ascii(logbuf_arry[i])) {
                next_line = TRUE;
                fputc(logbuf_arry[i], fp);
            }
        } else {
            if (next_line)
                fputc('\n', fp);
            next_line = FALSE;
        }
    }
    fprintf(fp, "\n");
    write_data_to_file("dmesg.data", logbuf_arry, log_buf_len);

exit:
    guest_client_release();
    return 0;
}
