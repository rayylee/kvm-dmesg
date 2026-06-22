project('kvm-dmesg', 'c', default_options : ['c_std=gnu99'])

# Compiler options
cflags = ['-Wall', '-Wextra', '-O2']

# Linker options
ldflags = ['-ldl']

# Architecture-specific source
host_arch = host_machine.cpu_family()
if host_arch == 'x86_64'
  arch_src = 'arch/x86_64.c'
elif host_arch == 'aarch64'
  arch_src = 'arch/aarch64.c'
else
  error('unsupported architecture: ' + host_arch)
endif

# Sources
sources = [
  'main.c',
  arch_src,
  'log.c',
  'version.c',
  'kernel.c',
  'global_data.c',
  'symbols.c',
  'printk.c',
  'xutil.c',
  'mem.c',
  'parse_hmp.c',
  'client.c',
  'libvirt_client.c',
  'qmp_client.c',
]

# Build executable
executable('kvm-dmesg',
  sources,
  c_args    : cflags,
  link_args : ldflags
)
