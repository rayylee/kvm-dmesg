# crash-utility 与 kvm-dmesg 实现对比分析

本文把 `crash-utility`（内核崩溃分析工具）的日志打印实现拉出来，与 `kvm-dmesg` 做对比，看看同样是从“裸内存”里把 `dmesg` 捞出来，两者在思路上有什么异同。

分析基于 crash-utility 源码（`crash-utility/crash`）中的以下文件：

- `kernel.c`：`log` / `dmesg` 命令入口，处理旧格式日志
- `printk.c`：处理新的 `printk_ringbuffer`（prb）
- `symbols.c`：符号表和 `vmcoreinfo` 解析
- `x86_64.c`：x86_64 架构初始化、地址翻译、KASLR 处理
- `arm64.c`：AArch64 架构初始化、地址翻译、KASLR 处理
- `memory.c` / `diskdump.c` / `dev.c`：内存读取后端

## 1. crash-utility 是什么

`crash-utility` 是一个通用的内核崩溃分析器，支持：

- kdump 生成的 `vmcore`
- `/proc/kcore`（live 系统内存）
- `/dev/crash`（live 系统，内核暴露的 raw 内存接口）
- `diskdump`、`sadump`、`kvmdump`、`vmss`、`xendump` 等多种 dump 格式
- 本地或远程内存源

它不仅能打印 `dmesg`，还能看任务列表、调用栈、内存、文件系统、网络等。打印 `dmesg` 只是其中一个命令（`log`）。

## 2. crash-utility 打印 dmesg 的流程

命令入口在 `kernel.c` 的 `cmd_log()`，它会依次尝试三种内核日志格式：

```c
if (kernel_symbol_exists("prb"))
    dump_lockless_record_log(msg_flags);          // printk.c
else if (kernel_symbol_exists("log_first_idx") &&
         kernel_symbol_exists("log_next_idx"))
    dump_variable_length_record_log(msg_flags);   // kernel.c
else
    dump_raw_log_buf(msg_flags);                  // kernel.c
```

这与 `kvm-dmesg` 的判断顺序完全一致。

### 2.1 prb（printk_ringbuffer）

`printk.c` 里的 `dump_lockless_record_log()` 与 `kvm-dmesg` 思路相同：

1. 读 `prb` 全局变量。
2. 从 `prb.desc_ring` 拿到描述符数组、info 数组、tail_id/head_id。
3. 从 `prb.text_data_ring` 拿到文本数据环。
4. 从 tail 到 head 遍历描述符，检查 `state_var` 的高 2 位，只输出 `committed` / `finalized` 记录。
5. 根据 `text_blk_lpos.begin/next` 取文本，跳过开头的 `desc_id`，按 `printk_info.text_len` 输出。

crash-utility 更完善的地方：

- 支持 `SHOW_LOG_CALLER`、`SHOW_LOG_LEVEL`、`SHOW_LOG_CTIME`、`SHOW_LOG_DICT` 等多种显示选项。
- 处理 Rust 调用栈的 demangle。
- 对文本换行、截断等边界情况处理得更细致。

### 2.2 variable-length record（struct log）

`kernel.c` 里的 `dump_variable_length_record_log()` 与 `kvm-dmesg` 基本相同：

- 读 `log_first_idx`、`log_next_idx`、`log_buf_len`、`log_buf`。
- 用 `log_from_idx()` / `log_next()` 在环形缓冲区上遍历。
- `dump_log_entry()` 解析 `struct log` 的 `ts_nsec`、`text_len` 等字段。

### 2.3 最老的 raw log_buf

如果前两种符号都不存在，crash-utility 会直接读 `log_buf` 数组，按字符环形缓冲区处理。`kvm-dmesg` 也有同样的兜底逻辑。

## 3. 符号解析与 vmcoreinfo

crash-utility 的符号表在 `symbols.c` 中维护。它的输入通常是：

- `vmlinux`（可选，带 debuginfo 最好）
- `System.map`
- dump 文件里的 ELF notes，尤其是 `VMCOREINFO`

### 3.1 为什么依赖 vmcoreinfo

crash-utility 大量读取 dump 文件中的 `VMCOREINFO` note。这个 note 里保存了崩溃时内核自动导出的关键常量和符号值，例如：

```text
SYMBOL(log_buf)=ffffffff8a123456
SYMBOL(log_buf_len)=ffffffff8a123460
SYMBOL(log_first_idx)=ffffffff8a123464
SYMBOL(log_next_idx)=ffffffff8a123468
SYMBOL(prb)=ffffffff8a123470
NUMBER(KERNEL_IMAGE_SIZE)=0x2000000
NUMBER(phys_base)=0x1000000
NUMBER(page_offset_base)=0xffff888000000000
NUMBER(VA_BITS)=48
NUMBER(kimage_voffset)=0xffff000008000000
OSRELEASE=6.6.0
SIZE(printk_info)=32
OFFSET(printk_info.ts_nsec)=0
OFFSET(printk_info.text_len)=16
```

`symbols.c` 和 `kernel.c` 中有专门的 `read_vmcoreinfo()` 接口，把这些字符串解析成数值。

### 3.2 与 kvm-dmesg 的对比

| | crash-utility | kvm-dmesg |
|---|---|---|
| 符号来源 | `vmlinux` + `System.map` + `vmcoreinfo` | 仅 `System.map` |
| 结构体布局 | `vmcoreinfo` 中的 `SIZE(...)` / `OFFSET(...)`，或 `vmlinux` debuginfo | `vmcoreinfo` 中的 `SIZE(...)` / `OFFSET(...)` |
| `log_buf` 等符号地址 | 优先用 `vmcoreinfo` 里的 `SYMBOL(...)` | 从 `System.map` 读链接地址，再用 KASLR 偏移修正 |

也就是说，crash-utility 把“运行时地址”这件事大量外包给了 `vmcoreinfo`，而 `kvm-dmesg` 因为拿不到完整的 `vmcoreinfo` 符号值，只能自己从 `System.map` 和寄存器/vmcoreinfo 推。

## 4. KASLR 与地址翻译

### 4.1 x86_64

在 `x86_64.c` 的 `x86_64_init()` 中：

1. **读取 KASLR 偏移**
   直接从 `vmcoreinfo` 读 `relocate`：
   ```c
   if ((string = pc->read_vmcoreinfo("relocate"))) {
       kt->relocate = htol(string, QUIET, NULL);
       kt->flags |= RELOC_SET;
       kt->flags2 |= KASLR;
   }
   ```
   如果 `vmcoreinfo` 里有这个字段，后续符号地址直接减去 `relocate` 即可。

2. **计算 phys_base**
   在 `x86_64_calc_phys_base()` 中，依次尝试：
   - `vmcoreinfo` 中的 `NUMBER(phys_base)`
   - 读取符号 `phys_base` 的值
   - 用 `_text` / `_stext` 等符号和 `__START_KERNEL_map` 推导

3. **地址翻译**
   `x86_64_kvtop()` 实现完整的页表 walk，支持：
   - 4 级 / 5 级页表
   - 2 MB / 1 GB huge page
   - Xen、KVM、EFI、SME 等特殊场景
   - `CONFIG_RANDOMIZE_MEMORY` 下的 `page_offset_base` 动态线性区

   简单地址也通过 `__START_KERNEL_map + phys_base` / `PAGE_OFFSET` 快速翻译，但复杂地址（vmalloc、modules、vmemmap）会走页表。

### 4.2 AArch64

在 `arm64.c` 的 `arm64_init()` 中：

1. **读取 kimage_voffset**
   优先从 `vmcoreinfo` 读 `NUMBER(kimage_voffset)`。
   如果是 live 系统，还可以通过 `/dev/crash` 的 `DEV_CRASH_ARCH_DATA` ioctl，或 `/proc/kallsyms` 获取。

2. **计算 PHYS_OFFSET 与 phys_base**
   从 `vmcoreinfo` 读 `NUMBER(PHYS_OFFSET)`，或用 `memstart_addr` 符号减去 `kimage_voffset`。

3. **地址翻译**
   `arm64_kvtop()`：
   - 线性映射区、内核映像区用 `kimage_voffset` / `PAGE_OFFSET` 简单翻译。
   - `vmalloc` 区域走页表 walk（`arm64_init_kernel_pgd()` 从 `init_mm.pgd` 或 `swapper_pg_dir` 拿到内核页表根）。

### 4.3 与 kvm-dmesg 的对比

| | crash-utility | kvm-dmesg |
|---|---|---|
| KASLR 来源 | `vmcoreinfo("relocate")` / `KERNELOFFSET` / `kimage_voffset` | x86_64：从 CR3/IDTR 推导；AArch64：扫描 vmcoreinfo |
| phys_base 来源 | `vmcoreinfo`、符号、`__START_KERNEL_map` 推导 | x86_64：IDTR 推导；AArch64：`kimage_vaddr - kimage_voffset` |
| 页表 walk | 完整支持，含 huge page、5-level、vmalloc | x86_64：只 walk IDTR 计算 KASLR；实际读内存用简单偏移；AArch64：不 walk |
| 架构覆盖 | x86、x86_64、arm64、ppc、ia64、s390 等 | 仅 x86_64 和 AArch64 |
| 鲁棒性 | 很高，能处理大量 corner case | 够用但有限（例如不处理 huge page） |

## 5. 内存读取后端

crash-utility 的 `readmem(addr, KVADDR, ...)` 隐藏了底层差异：

- **vmcore / diskdump**：把 dump 文件映射到内存，按内存段表（`pt_load` 或 diskdump 段表）找到对应物理页。
- **/proc/kcore**：读取 `/proc/kcore` 的 ELF 段。
- **/dev/crash**：直接 ioctl/read。
- **live qemu**：可通过 `kvmdump.c` 读取 QEMU 内存。

`kvm-dmesg` 的内存后端则简单得多：

- 优先 `/proc/<pid>/mem` + `gpa2hva`。
- 回退 QMP `xp` 命令或 libvirt 内存接口。
- 只面向运行中的 QEMU/KVM 虚拟机。

## 6. 代码组织与依赖

| | crash-utility | kvm-dmesg |
|---|---|---|
| 代码规模 | 约 20 万行 C | 约 2000 行 C |
| 依赖 | 需要 `vmlinux`/`System.map`/dump 文件；编译依赖 ncurses、lzo/snappy 等压缩库、gdb 等 | 只需要 `System.map` 和 QMP/libvirt；编译几乎无额外依赖 |
| 运行方式 | 交互式命令行工具 | 单次命令行工具 |
| 使用场景 | 崩溃分析、离线调查 | 运行中虚拟机快速抓 dmesg |

## 7. 总结

虽然 `crash-utility` 和 `kvm-dmesg` 最终都殊途同归——找到 `log_buf` / `prb`，按格式解码——但两者的设计哲学明显不同：

- **crash-utility**：
  > “我面对的是各种各样的 dump/live 源，必须尽可能通用和鲁棒。所以我依赖 `vmcoreinfo` 和完整的页表 walk，尽量覆盖所有内核版本和架构。”

- **kvm-dmesg**：
  > “我只做一件事：从运行中的 QEMU/KVM 虚拟机里抓 dmesg。所以我用 System.map + 寄存器/vmcoreinfo 快速算出 KASLR，用简单偏移读内存，保持代码最小化。”

对于 QEMU/KVM 运行中 guest 的快速排障，`kvm-dmesg` 更轻量；对于内核崩溃后的深度分析，`crash-utility` 是不可替代的工具。
