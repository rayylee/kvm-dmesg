# kvm-dmesg 实现原理：从寄存器到 dmesg

本文档描述 `kvm-dmesg` 如何从一台运行中的 KVM/QEMU 虚拟机里把内核的 `dmesg` 打印出来的基本流程。阅读对象是想要了解其内部实现的人。

## 1. 这个东西是干什么的

`kvm-dmesg` 不进入 guest、不依赖 guest 里的任何工具，只通过 host 上能访问到的 QEMU 接口（QMP socket、libvirt 或 `/proc/<pid>/mem`）和一份 guest 内核的 `System.map`，直接读取 guest 物理内存，找到内核的日志缓冲区并把它解码成人类可读的 `dmesg` 输出。

要做到这一点，核心需要解决三个问题：

1. **怎么读到 guest 内存？**
2. **内核加载地址是随机的（KASLR），怎么知道符号在内存里的实际位置？**
3. **找到日志缓冲区后，怎么按内核版本解析里面的记录？**

下面按顺序讲。

## 2. 整体流程

```text
+-----------------+     +---------------------+     +------------------+
| System.map      |     | QEMU / libvirt      |     | guest physical   |
| (符号链接地址)   |     | (CR3 / IDTR / 内存)  |     | memory           |
+--------+--------+     +----------+----------+     +--------+---------+
         |                         |                          |
         |                         v                          |
         |              +----------------------+               |
         |              | 计算 KASLR 偏移       |               |
         |              | (kaslr_offset /      |               |
         |              |  phys_base /         |               |
         |              |  kimage_voffset)     |               |
         |              +----------+-----------+               |
         |                         |                          |
         v                         v                          v
   +-----------------------------------------------+          |
   | 用 System.map 符号地址 - KASLR 偏移得到运行时地址  |          |
   +-----------------------------------------------+          |
                          |                                   |
                          v                                   |
   +-----------------------------------------------+          |
   | 把内核虚拟地址 (KVADDR) 转成 guest 物理地址 (paddr) |<---------+
   +-----------------------------------------------+
                          |
                          v
   +-----------------------------------------------+
   | 读取 log_buf / prb / vmcoreinfo_data 等数据结构  |
   +-----------------------------------------------+
                          |
                          v
   +-----------------------------------------------+
   | 解析 printk 记录格式并输出                      |
   +-----------------------------------------------+
```

## 3. 读取 guest 内存

`client.c` 把底层访问方式抽象成一套接口：

- `get_registers(idtr, cr3, cr4)`：获取 CPU 寄存器（仅 x86_64 需要）。
- `readmem(paddr, buf, size)`：按 guest 物理地址读取内存。

具体有三种 backend：

| 方式 | 怎么拿到 pid / 寄存器 | 怎么读内存 |
|------|----------------------|-----------|
| QMP socket | 通过 socket inode 在 `/proc` 里找到 QEMU pid；QMP `info registers` 取寄存器 | 优先用 `/proc/<pid>/mem` + `gpa2hva`；失败则回退到 QMP `xp` 命令 |
| libvirt domain | libvirt API 取 pid 和寄存器 | 同上，可回退到 libvirt 的内存 API |
| memory dump 文件 | 直接从文件读，寄存器由调用方/文件提供 | 直接文件映射/读取 |

> 为什么优先用 `/proc/<pid>/mem`？
> 因为 QMP 的 `xp` 命令一次只能读一小段，大数据量时非常慢；直接读 QEMU 进程的虚拟地址空间最快。

## 4. x86_64：从 CR3 / IDTR 算出 KASLR

### 4.1 为什么需要 CR3 和 IDTR

x86_64 Linux 内核编译时的链接地址是固定的，但启动时因为 KASLR 会被随机平移。要读任何内核符号，必须先知道这个平移量 `kaslr_offset`。

`CR3` 指向当前 CPU 的页表根（物理地址），`IDTR` 指向中断描述符表 IDT 的**虚拟地址**。IDT 本身在内核里，所以通过它可以验证内核的实际加载位置。

### 4.2 计算过程（在 `arch/x86_64.c` 中）

1. **拿到页表根**
   ```c
   pgd = cr3 & ~(CR3_PCID_MASK | PTI_USER_PGTABLE_MASK);
   ```
   去掉 PCID 和 PTI 用户页表位，得到真正的内核页目录物理地址。

2. **把 IDTR 虚拟地址翻译成物理地址**
   用刚刚得到的页表根 walk 页表，得到 `idtr_paddr`。这里走的是正常的 4 级页表。

3. **读 IDT 第 0 号门（divide_error）**
   IDT[0] 指向 `divide_error`（较新内核是 `asm_exc_divide_error`）处理函数。从 IDT 条目中拼出它的**运行时虚拟地址** `divide_error_vmcore`。

4. **算 KASLR 偏移**
   `System.map` 里记录了 `divide_error` 的**链接地址** `divide_error_vmlinux`。
   ```c
   kaslr_offset = divide_error_vmcore - divide_error_vmlinux;
   ```
   如果内核没有随机化，这个值就是 0。

5. **算 phys_base**
   `System.map` 里还有 `idt_table` 的链接地址。IDT 在物理内存里的位置也可以从 `idtr_paddr` 得到，从而推出内核映像的物理基址：
   ```c
   phys_base = idtr_paddr -
       (idt_table_vmlinux + kaslr_offset - __START_KERNEL_map);
   ```
   `__START_KERNEL_map` 是内核映像虚拟区的起始地址（`0xffffffff80000000`）。

### 4.3 读内存时的地址翻译

拿到 `kaslr_offset` 和 `phys_base` 后，任何内核虚拟地址都能转成物理地址：

- **内核映像区**（`>= __START_KERNEL_map`）：
  ```c
  paddr = (kvaddr - __START_KERNEL_map) + phys_base;
  ```
- **直接映射区/线性区**（低于 `__START_KERNEL_map`）：
  ```c
  paddr = kvaddr - PAGE_OFFSET;
  ```
  新内核的 `PAGE_OFFSET` 从 `page_offset_base` 符号动态获得。

这就是 `arch_kvtop()` 在 x86_64 下做的事情。

## 5. AArch64：没有页表寄存器怎么办

### 5.1 为什么不能像 x86 那样 walk 页表

QEMU 的 human monitor 命令在 AArch64 上**不暴露 `TTBR1_EL1`**，所以无法从 CR3 等价物拿到内核页表根。因此必须找别的办法把虚拟地址映射到物理地址。

### 5.2 扫描 vmcoreinfo

Linux 内核在 `vmcoreinfo` 里保存了一组崩溃转储时需要的关键常量和偏移。`kvm-dmesg` 在 guest 物理内存里扫描 `OSRELEASE=` 字符串，找到 `vmcoreinfo` 的一份拷贝，并验证它包含 `NUMBER(kimage_voffset)=`。

从这份数据里能直接拿到两个关键值：

- `kimage_voffset`：内核虚拟地址与物理地址的固定差值（`va - pa`）。
- `KERNELOFFSET`：KASLR 的随机化偏移量。

### 5.3 计算 KIMAGE_VADDR 与 phys_base

```c
kimage_vaddr = (_text + KERNELOFFSET) 向下对齐到 2 MB;
phys_base    = kimage_vaddr - kimage_voffset;
```

`_text` 来自 `System.map`，是内核映像的链接起始地址。

### 5.4 AArch64 的地址翻译

在 `arch/aarch64.c` 的 `aarch64_kvtop()` 里：

- **内核映像区**（`>= kimage_vaddr`）：
  ```c
  paddr = kvaddr - kimage_voffset;
  ```
- **线性映射区**（`>= PAGE_OFFSET`，通常是 `0xffff800000000000`）：
  ```c
  paddr = kvaddr - PAGE_OFFSET;
  ```
- **vmalloc / modules 等区域**：同样用 `kimage_voffset` 平移。

这样即使不做页表 walk，也能覆盖 dmesg 相关符号所在的区域。

## 6. 找到并解析日志缓冲区

地址翻译问题解决后，剩下的就是定位日志数据结构并按内核版本解析。

### 6.1 需要的符号

`symbols.c` 从 `System.map` 里只加载会用到的符号，主要包括：

- `log_buf`、`log_buf_len`
- `log_first_idx`、`log_next_idx`
- `prb`
- `vmcoreinfo_data`、`vmcoreinfo_size`
- `page_offset_base`
- x86_64 专用：`divide_error` / `asm_exc_divide_error`、`idt_table`
- AArch64 专用：`kimage_voffset`、`_text` / `_stext`

读取符号数据时，会先用 KASLR 偏移把链接地址修正成运行时地址，再调用 `readmem(KVADDR, ...)` 读内存。

### 6.2 三种 printk 记录格式

`main.c` 和 `printk.c` 按符号存在情况选择解析方式：

#### A. 最老的内核：原始 `log_buf`

直接把 `log_buf` 指向的内存按字符数组打印，遇到 `\0` 换行。适用于非常老的内核。

#### B. 可变长度记录（`struct log`）

内核 3.x ~ 5.x 左右使用。每个记录头包含：

- `ts_nsec`：纳秒时间戳
- `len`：整个记录长度
- `text_len`：文本长度

通过 `log_first_idx` / `log_next_idx` 在环形缓冲区上遍历，按记录头切分文本。

#### C. 无锁 printk ringbuffer（`prb`）

较新内核（5.10+，取决于发行版）使用 `printk_ringbuffer`。它分成两部分：

- **desc_ring**：描述符环，每个描述符保存一条记录的元数据（包括 `text_blk_lpos`）。
- **text_data_ring**：实际文本数据环。

`vmcoreinfo` 里保存了这些结构的大小和成员偏移：`SIZE(printk_ringbuffer)`、`OFFSET(prb_desc_ring)`、`SIZE(prb_desc)`、`OFFSET(printk_info.text_len)` 等等。`dump_lockless_record_log()` 先读 `prb`，再依次读 desc_ring、info_ring、text_data_ring，最后按 ID 从 tail 到 head 遍历输出。

### 6.3 版本信息

`kernel_init()` 从 `vmcoreinfo` 的 `OSRELEASE=` 字段解析出内核版本（例如 `6.6.119-...`），打印在日志最前面：

```text
Linux version: v6.6.119
```

## 7. 输出

解析每条记录时：

- 把 `ts_nsec` 转换成 `[ 123.456789]` 格式的时间戳。
- 对文本中的不可打印字符用 `.` 替换。
- 保持原有的换行。

最终输出与 guest 里 `dmesg` 看到的内容基本一致。

## 8. x86_64 与 AArch64 对比

| 步骤 | x86_64 | AArch64 |
|------|--------|---------|
| 获取寄存器 | QMP/libvirt `info registers` 取 CR3 / IDTR | 不需要 |
| 计算 KASLR | 用 IDT[0] 实际地址 - `divide_error` 符号地址 | 从 vmcoreinfo 读 `KERNELOFFSET` |
| 地址翻译 | `phys_base` + `PAGE_OFFSET` | `kimage_voffset` + `PAGE_OFFSET` |
| 页表 walk | 需要，仅用于把 IDTR 翻译成物理地址 | 不需要 |
| vmcoreinfo 用途 | 解析 `prb` 结构大小/偏移 + OSRELEASE | 同上，且用于获取 `kimage_voffset` |

## 9. 小结

`kvm-dmesg` 的核心思路可以概括为：

1. 通过 QEMU/libvirt 拿到 CPU 状态（x86_64）或扫描 vmcoreinfo（AArch64），解出 KASLR 和物理基址。
2. 利用 `System.map` 把符号的链接地址修正为运行时地址。
3. 把内核虚拟地址按架构规则转成 guest 物理地址并读取。
4. 根据内核版本选择对应的 printk 数据结构解析并输出。

因为所有信息都来自 host 侧可访问的接口，所以即使 guest 已经 hang 住、没有串口输出，也能把它的内核日志拖出来。
