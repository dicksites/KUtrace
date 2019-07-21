/*
 * kernel/kutrace/kutrace.c
 *
 * Copyright (C) 2019 Richard L. Sites
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * Small hooks for a module that implements kernel/user tracing
 * dsites 2019.02.14 Reworked for the 4.19 kernel from dclab_trace.c 
 *
 * See include/linux/kutrace.h for struct definitions
 *
 * Most patches will be something like
 *   kutrace1(event, arg)
 *
 */

#include <linux/kutrace.h>
#include <linux/kernel.h>
#include <linux/percpu.h>
#include <linux/types.h>

bool kutrace_tracing = false;
EXPORT_SYMBOL(kutrace_tracing);

struct kutrace_ops kutrace_global_ops = {NULL, NULL, NULL, NULL};
EXPORT_SYMBOL(kutrace_global_ops);

u64* kutrace_pid_filter = NULL;
EXPORT_SYMBOL(kutrace_pid_filter);

DEFINE_PER_CPU(struct kutrace_traceblock, kutrace_traceblock_per_cpu);
EXPORT_PER_CPU_SYMBOL(kutrace_traceblock_per_cpu);





