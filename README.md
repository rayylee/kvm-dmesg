# kvm-dmesg

`kvm-dmesg` is a tool designed to retrieve kernel logs from a guest virtual machine running under KVM, similar to the `dmesg` tool but for virtualized environments. This tool allows system administrators and developers to easily access the kernel logs of guest VMs directly from the host.

## Features

- **Guest Kernel Log Retrieval**: `kvm-dmesg` enables you to fetch kernel logs from KVM-based virtual machines.
- **Two Connection Methods**: It supports two different interfaces for log retrieval:
  - **libvirt Interface**: Fetch logs from VMs managed through the libvirt virtualization API.
  - **QMP Socket**: Connect directly to the QEMU monitor protocol (QMP) socket for communication.
- **x86_64 and AArch64 Linux VM Support**: Supports both x86_64 and AArch64 Linux virtual machines.
- **System.map Symbol Table**: `kvm-dmesg` requires access to the guest VM's `System.map` symbol table to map kernel log addresses to human-readable symbols.

## Compilation

To compile the tool, simply run:

```bash
$ make

or

$ meson setup build && meson compile -C build
```

## Usage

1. **Using libvirt**:
   ```bash
   $ ./kvm-dmesg <domain_name> <system.map_path>
   ```

2. **Using QMP Socket**:
   ```bash
   $ ./kvm-dmesg <socket_path> <system.map_path>
   ```

   In both commands, replace `<domain_name>` with the name of the virtual machine, `<socket_path>` with the path to the QMP socket, and `<system.map_path>` with the path to the `System.map` file for the guest kernel.

## Example

```bash
$ ./kvm-dmesg /tmp/qmp.sock System.map-5.15.171

[    0.000000] Linux version 5.15.171 (rayylee@ubuntu-dev) (gcc (Ubuntu 13.2.0-23ubuntu4) 13.2.0, GNU ld (GNU Binutils for Ubuntu) 2.42) #1 SMP Tue Nov 12 14:44:05 UTC 2024
[    0.000000] Command line: console=ttyS0
[    0.000000] BIOS-provided physical RAM map:
[    0.000000] BIOS-e820: [mem 0x0000000000000000-0x000000000009fbff] usable
[    0.000000] BIOS-e820: [mem 0x000000000009fc00-0x000000000009ffff] reserved
[    0.000000] BIOS-e820: [mem 0x00000000000f0000-0x00000000000fffff] reserved
[    0.000000] BIOS-e820: [mem 0x0000000000100000-0x000000001ffdffff] usable
[    0.000000] BIOS-e820: [mem 0x000000001ffe0000-0x000000001fffffff] reserved
[    0.000000] BIOS-e820: [mem 0x00000000feffc000-0x00000000feffffff] reserved
[    0.000000] BIOS-e820: [mem 0x00000000fffc0000-0x00000000ffffffff] reserved
[    0.000000] BIOS-e820: [mem 0x000000fd00000000-0x000000ffffffffff] reserved
[    0.000000] NX (Execute Disable) protection: active
[    0.000000] SMBIOS 2.8 present.
[    0.000000] DMI: QEMU Standard PC (i440FX + PIIX, 1996), BIOS rel-1.16.3-0-ga6ed6b701f0a-prebuilt.qemu.org 04/01/2014
[    0.000000] tsc: Fast TSC calibration using PIT
[    0.000000] tsc: Detected 3194.022 MHz processor
[    0.000569] e820: update [mem 0x00000000-0x00000fff] usable ==> reserved
[    0.000571] e820: remove [mem 0x000a0000-0x000fffff] usable
...
```

## Requirements

- Linux-based host with KVM support.
- Libvirt or access to the QMP socket for the virtual machine.
- The tool supports x86_64 and AArch64 Linux guests.
- The guest's `System.map` file must be available for symbol resolution.

## Acknowledgments

This tool is inspired by the functionality provided by the [Crash Utility](https://github.com/crash-utility/crash). Special thanks to the authors of this project for providing a robust set of tools for kernel crash analysis and debugging.

---

Let me know if you'd like to add or modify anything else!
