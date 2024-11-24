project('kvm-dmesg', 'c', default_options : ['c_std=gnu99'])

# Dependencies
libvirt_dep = dependency('libvirt')
libvirt_qemu_dep = dependency('libvirt-qemu')

# Compiler options
cflags = ['-Wall', '-Wextra', '-O2']

# Sources
sources = [
  'main.c',
  'log.c',
  'version.c',
  'kernel.c',
  'global_data.c',
  'symbols.c',
  'libvirt_client.c',
  'printk.c',
  'xutil.c',
  'qmp_client.c',
]

# Build executable
executable('kvm-dmesg',
  sources,
  dependencies : [libvirt_dep, libvirt_qemu_dep],
  c_args       : cflags
)
