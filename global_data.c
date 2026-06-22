/* global_data.c
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

#include "defs.h"

FILE *fp;

struct program_context program_context = { 0 };
struct program_context *pc = &program_context;

struct kernel_table kernel_table = { 0 };
struct kernel_table *kt = &kernel_table;

struct vm_table vm_table = { 0 };
struct vm_table *vt = &vm_table;

struct symbol_table_data symbol_table_data = { 0 };
struct symbol_table_data *st = &symbol_table_data;

struct machdep_table machdep_table = { 0 };
struct machdep_table *machdep = &machdep_table;

struct offset_table offset_table = { 0 };
struct size_table size_table = { 0 };

char *vmcoreinfo_buf = NULL;
size_t vmcoreinfo_size = 0;
