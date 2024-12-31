/* cmd.c
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
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

#include "xutil.h"
#include "log.h"
#include "defs.h"
#include "client.h"

struct cmd_struct {
    const char *cmd;
    int (*fn)(void);
};

static void write_data_to_file(const char *filename, void *data, size_t size) {
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

static int cmd_bpf()
{
    ulong start_btf_vmlinux = 0;
    ulong stop_btf_vmlinux = 0;
    ulong btf_size = 0;
    const char *vmlinux_btf = "vmlinux.btf";
    char *btf_buf;

    if (!kernel_symbol_exists("__start_BTF") ||
            !kernel_symbol_exists("__stop_BTF")) {
        pr_err("No BTF section found, the kernel version is too low.");
        return -1;
    }

    start_btf_vmlinux = relocate(symbol_value("__start_BTF"));
    stop_btf_vmlinux = relocate(symbol_value("__stop_BTF"));
    btf_size = stop_btf_vmlinux - start_btf_vmlinux;
    btf_size &= (MEGABYTES(512) - 1);

    btf_buf = xmalloc(btf_size);
    readmem(start_btf_vmlinux, KVADDR, btf_buf, btf_size);
    write_data_to_file(vmlinux_btf, btf_buf, btf_size);
    fprintf(fp, "Write BTF to %s file OK!\n", vmlinux_btf);

    return 0;
}

static struct cmd_struct *get_command(struct cmd_struct *command, const char *cmd)
{
    struct cmd_struct *p = command;

    while (cmd && p->cmd) {
        if (!strcmp(p->cmd, cmd))
            return p;
        p++;
    }
    return NULL;
}

static struct cmd_struct commands[] = {
    { "bpf", cmd_bpf },
    { NULL,  NULL    },
};

int handle_command(const char *cmd)
{
    struct cmd_struct *p = NULL;
    int ret = -1;

    if (cmd) {
        p = get_command(commands, cmd);
        if (!p) {
            pr_err("No support cmd %s", cmd);
            goto error;
        }
        ret = p->fn();
    }

error:
    return ret;
}
