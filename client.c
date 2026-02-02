#include "defs.h"
#include "xutil.h"
#include "mem.h"
#include "client.h"

guest_client_t *guest_client = NULL;

int get_cr3_idtr(uint64_t *cr3, uint64_t *idtr)
{
    uint64_t cr4;
    guest_client->get_registers(idtr, cr3, &cr4);
    return 0;
}

int readmem(uint64_t addr, int memtype, void *buffer, long size)
{
    physaddr_t paddr = 0;

    switch (memtype) {
        case KVADDR:
            if (addr >= __START_KERNEL_map) {
                paddr = ((addr) - (ulong)__START_KERNEL_map + machdep->machspec->phys_base);
            } else {
                paddr = ((addr) - PAGE_OFFSET);
            }
            break;
        case PHYSADDR:
            paddr = addr;
            break;
    }

    return guest_client->readmem(paddr, buffer, size);
}

int guest_client_new(char *ac, guest_access_t ty)
{
    if (guest_client)
        return 0;

    guest_client_t *c = xcalloc(1, sizeof(guest_client_t));
    c->ty = ty;
    switch(c->ty) {
        case GUEST_NAME:
            if (libvirt_client_init(ac))
                return -1;
            c->pid = libvirt_get_pid(ac);
            if (mem_init(c->pid, libvirt_gpa2hva) == 0) {
                c->readmem = mem_read;
            } else {
                c->readmem = libvirt_readmem;
            }
            c->get_registers = libvirt_get_registers;
            break;
        case GUEST_MEMORY:
            if (file_client_init(ac))
                return -1;
            c->get_registers = file_get_registers;
            c->readmem = file_readmem;
            break;
        case QMP_SOCKET:
            if (qmp_client_init(ac))
                return -1;
            c->pid = qmp_get_pid(ac);
            if (mem_init(c->pid, qmp_gpa2hva) == 0) {
                c->readmem = mem_read;
            } else {
                c->readmem = qmp_readmem;
            }
            c->get_registers = qmp_get_registers;
            break;
    }
    guest_client = c;
    return 0;
}

int guest_client_release()
{
    if (!guest_client)
        return 0;

    guest_client_t *c = guest_client;
    switch(c->ty) {
        case GUEST_NAME:
            libvirt_client_uninit();
            mem_uninit();
            break;
        case GUEST_MEMORY:
            file_client_uninit();
            break;
        case QMP_SOCKET:
            qmp_client_uninit();
            mem_uninit();
            break;
    }
    xfree(c);
    guest_client = NULL;

    return 0;
}
