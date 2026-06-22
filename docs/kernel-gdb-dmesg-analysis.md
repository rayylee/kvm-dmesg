# Linux 内核 GDB 脚本 `lx-dmesg` 的实现分析

Linux 内核源码树里自带了一套 GDB helper 脚本，放在 `scripts/gdb/linux/` 目录下。其中 `dmesg.py` 提供了 `lx-dmesg` 命令，作用与 `kvm-dmesg` 类似：把目标内核的 `dmesg` 打印出来。本文档分析它是如何实现的，并与 `kvm-dmesg` 的做法做对比。

## 1. 运行环境与前提

`lx-dmesg` 不是独立程序，而是运行在 GDB 内部的 Python 脚本。要使用它，必须满足以下条件：

1. **有一份带调试信息的 `vmlinux`**。
   GDB 需要从中读取符号地址、结构体大小和成员偏移。
2. **GDB 已经连接到能访问内核内存的 target**。
   常见场景：
   - QEMU 的 gdbstub（`qemu -s -S`）
   - KGDB（串口/以太网调试）
   - `gdb` 打开 kdump/vmcore 崩溃转储文件

在这些场景下，GDB 自己就已经知道内核符号、结构体布局，并且 target backend 会负责把内核虚拟地址翻译成实际可读的内存。所以 `dmesg.py` 完全不需要自己处理 KASLR、页表 walk、物理地址翻译这些事情。

## 2. 获取 `prb` 符号地址

现代内核使用 `printk_ringbuffer`（简称 `prb`）作为日志缓冲区。脚本首先要找到 `prb` 这个全局变量在内存里的地址：

```python
prb_addr = int(str(gdb.parse_and_eval("(void *)'printk.c'::prb")).split()[0], 16)
```

这里用了 GDB 的表达式解析：

- `'printk.c'::prb` 表示取 `printk.c` 源文件作用域里的 `prb` 符号。
- `(void *)` 把它转换成指针。
- 结果字符串形如 `"0xffff888123456789"`，split 后取第一个 token 转成整数。

**与 kvm-dmesg 的对比**：

| | `lx-dmesg` | `kvm-dmesg` |
|---|---|---|
| 符号地址来源 | GDB 解析 `vmlinux` 调试信息 | 解析 `System.map` 文本 |
| 是否需目标连接 | 是，GDB 已 attach 到 target | 不需要，通过 QMP/libvirt/`/proc/<pid>/mem` 直接读 |
| KASLR 处理 | GDB / target 已处理 | 自己从 CR3/IDTR 或 vmcoreinfo 计算 |
| 页表 walk | GDB / target 已处理 | x86_64 自己 walk；AArch64 不 walk |

## 3. 读取结构体布局信息

`dmesg.py` 不需要像 `kvm-dmesg` 那样去 `vmcoreinfo` 里查结构体大小和偏移。它直接用 GDB 的 type 查询接口：

```python
printk_info_type = utils.CachedType("struct printk_info")
prb_desc_type = utils.CachedType("struct prb_desc")
prb_desc_ring_type = utils.CachedType("struct prb_desc_ring")
prb_data_ring_type = utils.CachedType("struct prb_data_ring")
printfk_ringbuffer_type = utils.CachedType("struct printk_ringbuffer")
```

然后通过 `type['field'].bitpos // 8` 得到成员偏移，例如：

```python
off = printk_ringbuffer_type.get_type()['desc_ring'].bitpos // 8
```

**与 kvm-dmesg 的对比**：

- `kvm-dmesg` 为了兼容没有 DWARF 信息的运行环境，只能读 `vmcoreinfo` 里的 `SIZE(...)` / `OFFSET(...)` 字符串来推断结构布局。
- `lx-dmesg` 因为依赖 `vmlinux`，所以能直接拿到精确的类型信息，代码更简洁，也不需要处理 `vmcoreinfo_data` 指针的地址翻译。

## 4. 读取内存

脚本通过 `gdb.inferiors()[0].read_memory(addr, size)` 读取目标内存：

```python
inf = gdb.inferiors()[0]
prb = utils.read_memoryview(inf, prb_addr, sz).tobytes()
```

`utils.read_memoryview` 是对 `read_memory` 的薄包装，主要是为了兼容 Python 2/3 的 buffer 类型差异。

因为 target backend（QEMU gdbstub / KGDB / vmcore）已经能处理内核虚拟地址，脚本只需要传虚拟地址即可。

**与 kvm-dmesg 的对比**：

| | `lx-dmesg` | `kvm-dmesg` |
|---|---|---|
| 读内存接口 | `inferior.read_memory(kvaddr)` | `/proc/<pid>/mem` 或 QMP `xp` 或 libvirt |
| 地址类型 | 内核虚拟地址 | guest 物理地址 |
| 地址翻译 | target 负责 | 自己实现 `arch_kvtop()` |

## 5. 解析 `printk_ringbuffer`

拿到 `prb` 的原始字节后，解析步骤与 `kvm-dmesg` 基本一致：

1. 读 `prb` 本身。
2. 从 `prb.desc_ring` 拿到描述符环的地址、大小、个数。
3. 从 `prb.text_data_ring` 拿到文本数据环的地址、大小。
4. 读 `desc_ring.tail_id` 和 `desc_ring.head_id`。
5. 从 `tail_id` 遍历到 `head_id`：
   - 读取每个 `prb_desc`。
   - 检查 `state_var` 的高 2 位，只输出 `committed` 或 `finalized` 的记录。
   - 从 `text_blk_lpos.begin` / `next` 定位文本数据。
   - 跳过文本块开头的 `desc_id`（一个 `long`）。
   - 根据 `printk_info.text_len` 读取文本。
   - 把 `printk_info.ts_nsec` 转换成秒并输出。

关键代码片段：

```python
desc_committed = 1
desc_finalized = 2
desc_sv_bits = utils.get_long_type().sizeof * 8
desc_flags_shift = desc_sv_bits - 2

did = tail_id
while True:
    ind = did % desc_ring_count
    desc_off = desc_sz * ind
    info_off = info_sz * ind

    desc = utils.read_memoryview(inf, desc_addr + desc_off, desc_sz).tobytes()

    state = 3 & (utils.read_atomic_long(desc, sv_off) >> desc_flags_shift)
    if state != desc_committed and state != desc_finalized:
        if did == head_id:
            break
        did = (did + 1) & desc_id_mask
        continue

    begin = utils.read_ulong(desc, begin_off) % text_data_sz
    end = utils.read_ulong(desc, next_off) % text_data_sz

    info = utils.read_memoryview(inf, info_addr + info_off, info_sz).tobytes()

    if begin & 1 == 1:
        text = ""
    else:
        if begin > end:
            begin = 0
        text_start = begin + utils.get_long_type().sizeof
        text_len = utils.read_u16(info, len_off)
        if end - text_start < text_len:
            text_len = end - text_start
        text_data = utils.read_memoryview(inf, text_data_addr + text_start,
                                          text_len).tobytes()
        text = text_data[0:text_len].decode(encoding='utf8', errors='replace')

    time_stamp = utils.read_u64(info, ts_off)
    for line in text.splitlines():
        msg = u"[{time:12.6f}] {line}\n".format(
            time=time_stamp / 1000000000.0, line=line)
        gdb.write(msg)

    if did == head_id:
        break
    did = (did + 1) & desc_id_mask
```

## 6. 不支持旧格式

当前 `dmesg.py` 只实现了对 `printk_ringbuffer`（prb）的解析，没有处理：

- 最老的原始 `log_buf` 环形缓冲区。
- `struct log` 可变长度记录格式（`log_first_idx` / `log_next_idx`）。

因此它只能用于较新的内核。如果目标内核太老，`prb` 符号不存在，`gdb.parse_and_eval` 会直接报错。

`kvm-dmesg` 则同时支持三种格式，按符号存在情况自动选择：

1. `prb` 存在 → 解析 lockless ringbuffer。
2. `log_first_idx` / `log_next_idx` 存在 → 解析 `struct log`。
3. 否则 → 直接按字符数组打印 `log_buf`。

## 7. 各自适用的场景

| 场景 | 推荐工具 |
|---|---|
| 已有 `vmlinux` 调试信息，且 GDB 已连到 QEMU/KGDB/vmcore | `lx-dmesg` 更省事 |
| 只有 `System.map`，guest 还在运行，没有 vmlinux 调试信息 | `kvm-dmesg` |
| 需要兼容老内核（3.x / 4.x） | `kvm-dmesg` |
| 需要脚本化、批量处理、不启动 GDB | `kvm-dmesg` |
| AArch64 运行中 guest | `kvm-dmesg`（`lx-dmesg` 也能工作，但需 GDB 环境） |

## 8. 小结

`lx-dmesg` 的解决思路可以概括为：

> **把地址翻译、KASLR、符号解析、结构体布局这些脏活都交给 GDB + target backend，脚本只负责按 `printk_ringbuffer` 的格式把日志解码出来。**

它的优势是代码简洁、结构信息精确、不需要 `System.map`；劣势是必须依赖 GDB 调试环境和带 DWARF 的 `vmlinux`，且只支持新内核的 prb 格式。

`kvm-dmesg` 则走了另一条路：

> **不依赖 GDB，只通过 host 上可得的接口（QMP/libvirt/proc）和 `System.map`，自己完成 KASLR 计算、地址翻译、结构解析。**

这条路更通用、更独立，但也因此代码更复杂，需要分别为 x86_64 和 AArch64 处理地址翻译，并且要从 `vmcoreinfo` 推断结构体布局。
