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
    kvtop(idtr, &idtr_paddr);

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

    if (KDEBUG(1)) {
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

    if (!file) {
        pr_err("Failed to open file: %s", path);
        return -1;
    }

    while (bytes_read < 1024 && fread(&byte, 1, 1, file) == 1) {
        if (!isprint(byte) && !isspace(byte) && byte != '\0') {
            fclose(file);
            return 0;
        }
        bytes_read++;
    }

    fclose(file);
    return 1;
}

static void usage(void)
{
    fprintf(fp, "kvm-dmesg version %s \n", get_version_text());
    fprintf(fp, "Print the kernel messages from a virtual machine running under KVM\n");
    fprintf(fp, "\n");
    fprintf(fp, "Usage: kvm-dmesg <domain_name/socket_path> <system.map> [options]\n");
    fprintf(fp, "\n");
    fprintf(fp, "  -h, --help       display this help and exit\n");
    fprintf(fp, "  -v, --version    output version information and exit\n");
    fprintf(fp, "  -d, --debug      specify debug level\n");
    fprintf(fp, "\n");
}

static void version(void)
{
    fprintf(fp, "Version %s\n", get_version_text());
}

static int parse_options(int argc, char **argv)
{
    int ch;
    int idx = 0;
    const char *short_opts = "hvd:";
    static const struct option long_opts[] = {
        {"help",      no_argument,       NULL, 'h'},
        {"version",   no_argument,       NULL, 'v'},
        {"debug",     required_argument, NULL, 'd'},
        {NULL,        0,                 NULL, 0  }
    };

    while ((ch = getopt_long(argc, argv, short_opts, long_opts, &idx)) != -1) {
        switch (ch) {
            case 'h':
                usage();
                exit(0);
            case 'v':
                version();
                exit(0);
            case 'd':
                pc->debug = atol(optarg);
                if (pc->debug > 0) {
                    log_init(LOGLEVEL_DEBUG);
                }
                break;
            case '?':
                fprintf(fp, "Try `%s --help' for more information.\n", argv[0]);
                exit(0);
                break;
        }
    }

    return optind;
}

int main(int argc, char *argv[])
{
    char *arg1 = NULL;
    char *arg2 = NULL;
    struct stat path_stat;
    char *symmap_file = NULL;
    char *guest_ac = NULL;
    guest_access_t ac_type;
    int ind;

    pc->debug = 0;
    fp = stdout;

    ind = parse_options(argc, argv);
    if (ind < argc) {
        arg1 = argv[ind];
        ind++;
    }
    if (ind < argc) {
        arg2 = argv[ind];
        ind++;
    }

    if (!stat(arg1, &path_stat) && S_ISREG(path_stat.st_mode)) {
        if (is_text_file(arg1) == 1) {
            symmap_file = arg1;
            guest_ac = arg2;
        }
    }
    if (!symmap_file) {
        if (!stat(arg2, &path_stat) && S_ISREG(path_stat.st_mode)) {
            if (is_text_file(arg2) == 1) {
                symmap_file = arg2;
                guest_ac = arg1;
            }
        }
    }
    if (!symmap_file) {
        pr_err("System.map file not found");
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

    if (KDEBUG(1)) {
        pr_debug("Guest     : %s", guest_ac);
        pr_debug("System.map: %s", symmap_file);
    }

    if (guest_client_new(guest_ac, ac_type))
        return -1;
    symtab_init(symmap_file);
    x86_64_init();
    derive_kaslr_offset();
    x86_64_post_reloc();

    vmcoreinfo_init();
    kernel_init();

    if (kernel_symbol_exists("prb")) {
        dump_lockless_record_log();
        goto exit;
    } else if (kernel_symbol_exists("log_first_idx") &&
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

    if (KDEBUG(1)) {
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
