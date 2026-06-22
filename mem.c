/* mem.c
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
#include <unistd.h>
#include <stdint.h>
#include <fcntl.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "log.h"
#include "xutil.h"
#include "mem.h"

static proc_mem_t *proc_mem = NULL;

int mem_init(pid_t pid, int (*gpa2hva)(uint64_t, uint64_t*))
{
    int fd;
    char mem_path[32];
    if (proc_mem && proc_mem->mem_fd > 0)
        return 0;

    snprintf(mem_path, sizeof(mem_path), "/proc/%d/mem", pid);
    fd = open(mem_path, O_RDONLY);
    if (fd == -1) {
        return -1;
    }

    if (!proc_mem) {
        proc_mem = (proc_mem_t *)xmalloc(sizeof(proc_mem_t));
    }

    proc_mem->mem_fd = fd;
    proc_mem->pid = pid;
    proc_mem->gpa2hva = gpa2hva;

    return 0;
}

int mem_uninit()
{
    if (!proc_mem) {
        return 0;
    }
    if (proc_mem->mem_fd > 0) {
        close(proc_mem->mem_fd);
    }
    xfree(proc_mem);
    return 0;
}

int mem_read(uint64_t addr, void *buffer, size_t size)
{
    uint64_t hva;

    if (!proc_mem || proc_mem->mem_fd <= 0)
        return -1;

    if (!proc_mem->gpa2hva)
      return -1;

    /*
     * When the memory is greater than 4GB, the virtual machine's memory is not
     * contiguous in the QEMU's address space, so it is always necessary to
     * calculate the HVA based on the GPA.
     */
    if (proc_mem->gpa2hva(addr, &hva) < 0)
      return -1;

    if (lseek(proc_mem->mem_fd, hva, SEEK_SET) == -1) {
      pr_err("Failed to seek to the specified memory address");
      return -1;
    }

    ssize_t bytes_read = xread(proc_mem->mem_fd, buffer, size);
    if (bytes_read == -1) {
        pr_err("Failed to read memory");
        return -1;
    }

    return 0;
}


/*
 * Simple memmem replacement that does not require _GNU_SOURCE.
 */
static void *mem_mem(const void *haystack, size_t haystack_len,
                     const void *needle, size_t needle_len)
{
    const char *h = haystack;
    const char *n = needle;

    if (needle_len == 0 || haystack_len < needle_len)
        return NULL;

    for (size_t i = 0; i <= haystack_len - needle_len; i++) {
        if (memcmp(h + i, n, needle_len) == 0)
            return (void *)(h + i);
    }
    return NULL;
}

/*
 * Scan QEMU's guest RAM (via /proc/<pid>/mem) to find a copy of vmcoreinfo.
 * This is needed on AArch64 where QEMU does not expose TTBR1_EL1 through the
 * monitor interface, so we cannot do a page table walk.  vmcoreinfo contains
 * kimage_voffset and KERNELOFFSET which are sufficient to translate kernel
 * virtual addresses to guest physical addresses.
 */
int mem_scan_vmcoreinfo(char *buf, size_t buf_size)
{
    char maps_path[64];
    FILE *maps_fp;
    char line[512];
    uint64_t hva_start = 0, hva_end = 0;
    int found_region = 0;
    int mem_fd;

    if (!proc_mem || proc_mem->pid <= 0)
        return -1;

    snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps", proc_mem->pid);
    maps_fp = fopen(maps_path, "r");
    if (!maps_fp)
        return -1;

    while (fgets(line, sizeof(line), maps_fp)) {
        if (strstr(line, "memory-backend-memfd")) {
            if (sscanf(line, "%lx-%lx", &hva_start, &hva_end) == 2) {
                found_region = 1;
                break;
            }
        }
    }
    fclose(maps_fp);

    if (!found_region) {
        pr_err("Could not find guest RAM region in /proc/%d/maps", proc_mem->pid);
        return -1;
    }

    mem_fd = proc_mem->mem_fd;

#define VMCORE_SCAN_CHUNK (1 << 20)
#define VMCORE_MAX_SIZE   (1 << 12)
    char *chunk = malloc(VMCORE_SCAN_CHUNK + VMCORE_MAX_SIZE);
    if (!chunk)
        return -1;

    int ret = -1;
    const char *needle = "OSRELEASE=";
    const char *valid_marker = "NUMBER(kimage_voffset)=";
    size_t needle_len = strlen(needle);
    size_t marker_len = strlen(valid_marker);
    uint64_t pos = hva_start;

    while (pos + VMCORE_MAX_SIZE < hva_end) {
        size_t to_read = VMCORE_SCAN_CHUNK;
        if (pos + to_read > hva_end)
            to_read = hva_end - pos;

        if (lseek(mem_fd, pos, SEEK_SET) == (off_t)-1) {
            pos += VMCORE_SCAN_CHUNK;
            continue;
        }

        ssize_t n = xread(mem_fd, chunk, to_read + VMCORE_MAX_SIZE);
        if (n < (ssize_t)needle_len) {
            pos += VMCORE_SCAN_CHUNK;
            continue;
        }

        size_t search_len = (n > VMCORE_SCAN_CHUNK) ? VMCORE_SCAN_CHUNK : n;
        char *p = chunk;
        while ((p = mem_mem(p, search_len - (p - chunk), needle, needle_len)) != NULL) {
            size_t offset = p - chunk;
            size_t copy_len = VMCORE_MAX_SIZE;
            if (offset + copy_len > (size_t)n)
                copy_len = n - offset;
            if (copy_len > buf_size)
                copy_len = buf_size;

            /*
             * vmcoreinfo has several copies in RAM.  Keep looking until we
             * find one that contains kimage_voffset.
             */
            if (copy_len > marker_len &&
                mem_mem(p, copy_len, valid_marker, marker_len)) {
                memcpy(buf, p, copy_len);
                if (copy_len < buf_size)
                    buf[copy_len] = '\0';
                ret = 0;
                break;
            }
            p += needle_len;
        }
        if (ret == 0)
            break;

        pos += VMCORE_SCAN_CHUNK;
    }

    free(chunk);
    return ret;
#undef VMCORE_SCAN_CHUNK
#undef VMCORE_MAX_SIZE
}
