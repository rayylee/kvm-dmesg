/* client.h
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

#ifndef __CLIENT_H__
#define __CLIENT_H__

#include <stdint.h>
#include <stddef.h>

typedef enum {
    GUEST_NAME,
    GUEST_MEMORY,
    QMP_SOCKET,
} guest_access_t;

typedef struct {
    guest_access_t ty;
    pid_t pid;
    int (*get_registers)(uint64_t*, uint64_t*, uint64_t*);
    int (*readmem)(uint64_t, void*, size_t);
} guest_client_t;

int get_cr3_idtr(uint64_t *cr3, uint64_t *idtr);
int readmem(uint64_t addr, int memtype, void *buffer, long size);

int guest_client_new(char *ac, guest_access_t ty);
int guest_client_release();

int qmp_client_init(char *sock_path);
int qmp_client_uninit();
int qmp_get_registers(uint64_t *idtr, uint64_t *cr3, uint64_t *cr4);
int qmp_readmem(uint64_t addr, void *buffer, size_t size);
pid_t qmp_get_pid(char *sock_path);
int qmp_gpa2hva(uint64_t gpa, uint64_t *hva);

int libvirt_client_init(char *guest_name);
int libvirt_client_uninit();
int libvirt_get_registers(uint64_t *idtr, uint64_t *cr3, uint64_t *cr4);
int libvirt_readmem(uint64_t addr, void *buffer, size_t size);
pid_t libvirt_get_pid(char *guest_name);
int libvirt_gpa2hva(uint64_t gpa, uint64_t *hva);

int file_client_init(char *sock_path);
int file_client_uninit();
int file_get_registers(uint64_t *idtr, uint64_t *cr3, uint64_t *cr4);
int file_readmem(uint64_t addr, void *buffer, size_t size);

#endif
