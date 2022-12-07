/*
 * kernel/kutrace/kutrace.c
 *
 * Author: Richard Sites <dick.sites@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Signed-off-by: Richard Sites <dick.sites@gmail.com>
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

/* 2020.10.28 */
struct kutrace_nf kutrace_net_filter = {0LLU, {0LLU, 0LLU, 0LLU}};
EXPORT_SYMBOL(kutrace_net_filter);

DEFINE_PER_CPU(struct kutrace_traceblock, kutrace_traceblock_per_cpu);
EXPORT_PER_CPU_SYMBOL(kutrace_traceblock_per_cpu);





