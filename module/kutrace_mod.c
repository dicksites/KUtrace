/*
 * kutrace_mod.c
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
 * A module that implements kernel/user tracing
 * dsites 2019.02.19
 *
 * See include/linux/kutrace.h for struct definitions
 *
 * Most patches will be something like
 *   kutrace1(event, arg)
 *
 */

/*
 * kutrace.c -- kernel/user tracing implementation
 * dsites 2019.02.14 Reworked for the 4.19 kernel, from dclab_trace.c 
 *
 */

#include <linux/kutrace.h>

#include <linux/build_bug.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/percpu.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/stat.h>
#include <linux/string.h>
#include <linux/types.h>	/* u64, among others */
#include <linux/vmalloc.h>
#include <asm/atomic.h>
#include <asm/uaccess.h>


MODULE_LICENSE("GPL");
MODULE_AUTHOR("Richard L Sites");

/* Forward declarations */
static u64 kutrace_control(u64 command, u64 arg);
static int __init kutrace_mod_init(void);

/* For the flags byte in traceblock[1] */
#define IPC_Flag 0x80ul
#define WRAP_Flag 0x40ul

/* Incoming arg to do_reset  */
#define DO_IPC 1
#define DO_WRAP 2

/* Module parameter: default how many MB of kernel trace memory to reserve */
/* This is for the standalone, non-module version */
/* static const long int kTraceMB = 32; */

/* Version number of this kernel tracing code */
static const u64 kModuleVersionNumber = 3;


/* IPC Inctructions per cycle flag */
static bool do_ipc;	/* Initially false */

/* Wraparound tracing vs. stop when buffer is full */
static bool do_wrap;	/* Initially false */

/* Module parameter: default how many MB of kernel trace memory to reserve */
static long int tracemb = 2;

module_param(tracemb, long, S_IRUSR);
MODULE_PARM_DESC(tracemb, "MB of kernel trace memory to reserve");


/* These four are exported by our patched kernel. 
 * See linux-4.19.19/kernel/kutrace/kutrace.c
 */
extern bool kutrace_tracing;
extern struct kutrace_ops kutrace_global_ops;
extern u64* kutrace_pid_filter;
DECLARE_PER_CPU(struct kutrace_traceblock, kutrace_traceblock_per_cpu);


/*
 * Individual trace entries are at least one u64, with this format:
 *
 *  +-------------------+-----------+-------+-------+-------+-------+
 *  | timestamp         | event     | delta | retval|      arg0     |
 *  +-------------------+-----------+-------+-------+-------+-------+
 *           20              12         8       8           16
 *
 * timestamp: low 20 bits of some free-running time counter in the
 *   10-40 MHz range. For ARM, this is the 32 MHz cntvct_el0.
 * event: traced event number, syscall N, sysreturn N, etc.
 *   See user-mode kutrace_lib.h for the full set.
 *   matching call and return events differ just in one event bit.
 * delta: for optimized call-return, return timestamp - call timestamp,
 *   else zero.
 * retval: for optimized call-return, the low 8 bits of the return value,
 *   else zero.
 * arg0: for syscall, the low 16 bits of the first argument to the syscall,
 *   else zero.
 *
 * Multi-u64 entries have a count 1-8 in the middle 4 bits of event.
 *   These events are all in the range 0x000 to 0x1ff with the middle
 *   four bits non-zero.
 *
 * The first word of each 64KB block has this format:
 *  +-------+-------------------------------------------------------+
 *  |  cpu# |  full timestamp                                       |
 *  +-------+-------------------------------------------------------+
 *        56                                                       0
 *
 * The second word of each 64KB block has this format:
 *  +-------+-------------------------------------------------------+
 *  | flags |  gettimeofday() value to be filled in by user code    |
 *  +-------+-------------------------------------------------------+
 *        56                                                       0
 *
 */

#define ARG0_MASK      0x000000000000ffffl
#define RETVAL_MASK    0x0000000000ff0000l
#define DELTA_MASK     0x00000000ff000000l
#define EVENT_MASK     0x00000fff00000000l
#define TIMESTAMP_MASK 0xfffff00000000000l
#define EVENT_DELTA_RETVAL_MASK (EVENT_MASK | DELTA_MASK | RETVAL_MASK)
#define EVENT_RETURN_BIT           0x0000020000000000l
#define EVENT_LENGTH_FIELD_MASK 0x000000000000000fl

#define UNSHIFTED_RETVAL_MASK 0x00000000000000ffl
#define UNSHIFTED_DELTA_MASK  0x00000000000000ffl
#define UNSHIFTED_EVENT_MASK  0x0000000000000fffl
#define UNSHIFTED_TIMESTAMP_MASK 0x00000000000fffffl
#define UNSHIFTED_EVENT_RETURN_BIT 0x0000000000000200l
#define UNSHIFTED_EVENT_HAS_RETURN_MASK 0x0000000000000c00l

#define MIN_EVENT_WITH_LENGTH 0x010l
#define MAX_EVENT_WITH_LENGTH 0x1ffl
#define MAX_DELTA_VALUE 255
#define MAX_PIDNAME_LENGTH 16

#define RETVAL_SHIFT 16
#define DELTA_SHIFT 24
#define EVENT_SHIFT 32
#define TIMESTAMP_SHIFT 44
#define EVENT_LENGTH_FIELD_SHIFT 4

#define FULL_TIMESTAMP_MASK 0x00ffffffffffffffl
#define CPU_NUMBER_SHIFT 56

#define GETTIMEOFDAY_MASK 0x00ffffffffffffffl
#define FLAGS_SHIFT 56


/*
 * Trace memory is consumed backward, high to low
 * This allows valid test for full block even if an interrupt routine
 * switches to a new block mid-test. The condition tracebase == NULL
 * means that initialization needs to be called.
 *
 * Per-CPU trace blocks are 64KB, contining 8K u64 items. A trace entry is
 * 1-8 items. Trace entries do not cross block boundaries.
 *
 */
char *tracebase;	/* Initially NULL address of kernel trace memory */
u64 *traceblock_high;		/* just off high end of trace memory */
u64 *traceblock_limit;		/* at low end of trace memory */
u64 *traceblock_next;		/* starts at high, moves down to limit */
bool did_wrap_around;

/*
 * Trace memory layout without IPC tracing.
 *  tracebase
 *  traceblock_limit          traceblock_next                traceblock_high
 *  |                               |                                |
 *  v                               v                                v
 *  +-------+-------+------+--------+-------+-------+-------+-------+
 *  | / / / / / / / / / / / / / / / |                               |
 *  +-------+-------+------+--------+-------+-------+-------+-------+
 *                                  <==== allocated blocks grow down
 *
 *
 * Trace memory layout with IPC tracing. IPC bytes go into lower 1/8.
 *  tracebase
 *  |    traceblock_limit     traceblock_next                traceblock_high
 *  |       |                       |                                |
 *  v       v                       v                                v
 *  +-------+-------+------+--------+-------+-------+-------+-------+
 *  |////|  | / / / / / / / / / / / |                               |
 *  +-------+-------+------+--------+-------+-------+-------+-------+
 *       <==                        <==== allocated blocks grow down
 *       IPC bytes
 */

DEFINE_RAW_SPINLOCK(kutrace_lock);

/* Trace block size in bytes = 64KB */
#define KUTRACEBLOCKSHIFT (16)
#define KUTRACEBLOCKSIZE (1 << KUTRACEBLOCKSHIFT)

/* Trace block size in u64 words */
#define KUTRACEBLOCKSHIFTU64 (KUTRACEBLOCKSHIFT - 3)
#define KUTRACEBLOCKSIZEU64 (1 << KUTRACEBLOCKSHIFTU64)

/* IPC block size in u8 bytes */
#define KUIPCBLOCKSHIFTU8 (KUTRACEBLOCKSHIFTU64 - 3)
#define KUIPCBLOCKSIZEU8 (1 << KUIPCBLOCKSHIFTU8)

/* IPC design */
/* Map IPC * 8 [0.0 .. 3.75] into sorta-log value */
static const u64 kIpcMapping[64] = {
  0,1,2,3, 4,5,6,7, 8,8,9,9, 10,10,11,11, 
  12,12,12,12, 13,13,13,13, 14,14,14,14, 15,15,15,15,
  15,15,15,15, 15,15,15,15, 15,15,15,15, 15,15,15,15,
  15,15,15,15, 15,15,15,15, 15,15,15,15, 15,15,15,15
};

#if defined(__aarch64__)
	BUILD_BUG_ON_MSG(1, "Define get_inst_retired for aarch64");
#elif defined(__x86_64__)
/* RDMSR Read a 64-bit value from a MSR. */
/* The A constraint stands for concatenation of registers EAX and EDX. */
inline u64 rdMSR(u32 msr) {
   u32 lo, hi;
   asm volatile( "rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr) );
   return ((u64)lo) | (((u64)hi) << 32);
}

/* WRMSR Write a 64-bit value to a MSR. */
/* The A constraint stands for concatenation of registers EAX and EDX. */
inline void wrMSR(u32 msr, u64 value)
{
  u32 lo = value;
  u32 hi = value >> 32;
  asm volatile( "wrmsr" : : "a"(lo), "d"(hi), "c"(msr) );
}


/* NOTE: this is Intel x86-64 specific. It crashes on AMD */
/* NOTE: the Intel values seem bogus */
/* We want to use the fixed registers */
inline u64 get_inst_retired_intel(void) {
    u32 a = 0, d = 0;
    int ecx = 0x309; 	/* What counter it selects, Intel */
    __asm __volatile("rdmsr" : "=a"(a), "=d"(d) : "c"(ecx));
    return ((u64)a) | (((u64)d) << 32);
}

inline u64 get_inst_retired_amd(void) {
    u32 a = 0, d = 0;
    int ecx = 0xc00000e9; 	/* What counter it selects, AMD */
    __asm __volatile("rdmsr" : "=a"(a), "=d"(d) : "c"(ecx));
    return ((u64)a) | (((u64)d) << 32);
}


/* Set up enables for get_inst_retired */
void setup_get_inst_retired_intel(void) {
	u64 inst_ret_ctrl;
	u64 inst_ret_enable;
	/* cpuCount_HW_INSTRUCTIONS = 1<<30 */
	/* Configure fixed inst_ret counter in IA32_FIXED_CTR_CTRL 0x38D */
	/*   count both kernel and user, 0=count per-CPU-thread, no interrupt */
	/* msr[ 0x38D] &= ~0x000000000000000Flu; */
	/* msr[ 0x38D] |=  0x0000000000000003lu; */
	inst_ret_ctrl = rdMSR(0x38D);
printk(KERN_INFO "kutrace_ipc_mod rdMSR(0x38D) = %016llx\n", inst_ret_ctrl);
	inst_ret_ctrl &= ~0x000000000000000Flu;
	inst_ret_ctrl |=  0x0000000000000003lu;
	wrMSR(0x38D, inst_ret_ctrl);

	/* Enable fixed inst_ret counter in IA32_PERF_GLOBAL_CTRL 0x38F */
	/* msr[ 0x38F] |= 0x0000000200000000lu; */
	inst_ret_enable = rdMSR(0x38F);
printk(KERN_INFO "kutrace_ipc_mod rdMSR(0x38F) = %016llx\n", inst_ret_enable);
	inst_ret_enable |= (1lu << 32);
	wrMSR(0x38F, inst_ret_enable);
}

void setup_get_inst_retired_amd(void) {
	u64 inst_ret_enable;
	/* Enable fixed inst_ret counter in IA32_PERF_GLOBAL_CTRL 0x38F */
	/* msr[ 0x38F] |= 0x0000000200000000lu; */
	inst_ret_enable = rdMSR(0xC0010015);
printk(KERN_INFO "kutrace_ipc_mod rdMSR(0xC0010015) = %016llx\n", inst_ret_enable);
	inst_ret_enable |= (1lu << 30);
	wrMSR(0xC0010015, inst_ret_enable);
}

/* choose later */
inline u64 get_inst_retired(void) {return get_inst_retired_amd();}

/* choose later */
inline void setup_get_inst_retired(void) {setup_get_inst_retired_amd();}



#else
	BUILD_BUG_ON_MSG(1, "Define get_inst_retired for your architecture");
	timer_value = 0;
#endif


/* Map IPC= inst_retired / cycles to sorta-log four bits */
/* NOTE: delta_cycles is in increments of cycles/64. The arithmetic */
/*       below compensates for this. */
/* 0, 1/8, 1/4, 3/8,  1/2, 5/8, 3/4, 7/8,  1, 5/4, 3/2, 7/4,  2, 5/2, 3, 7/2 */
inline u64 get_granular(u64 delta_inst, u64 delta_cycles) {
  u32 del_inst, del_cycles, ipc;
  if ((delta_cycles & ~1) == 0) return 0; /* Too small to matter; avoid zdiv */
  /* Do 32-bit divide to save ~10 CPU cycles vs. 64-bit */
  /* With ~20ms guaranteed max interval, no overflow problems */
  del_inst = (u32)delta_inst;
  del_cycles = (u32)(delta_cycles << 3);  /* cycles/64 to cycles/8 */
  ipc = del_inst / del_cycles;	          /* gives IPC*8 */
  return kIpcMapping[ipc & 0x3F];	  /* Truncate unexpected IPC >= 8.0 */
}


/*x86-64 or Arm-specific timer */
/* Arm returns 32MHz counts: 31.25 ns each */
/* x86-64 version returns rdtsc() >> 6 to give ~20ns resolution */
/* In both cases, ku_get_cycles returns cycles/64, not cycles */
inline u64 ku_get_cycles(void)
{
	u64 timer_value;
#if defined(__aarch64__)
	asm volatile("mrs %0, cntvct_el0" : "=r"(timer_value));
#elif defined(__x86_64__)
	timer_value = rdtsc() >> 6;
#else
	BUILD_BUG_ON_MSG(1, "Define the time base for your architecture");
	timer_value = 0;
#endif
	return timer_value;
}

/* Make sure name length fits in 1..8 u64's */
//* Return true if out of range */
inline bool is_bad_len(int len)
{
	return (len < 1) | (len > 8);
}



/* Turn off tracing. (We cannot wait here) */
/* Return tracing bit */
static u64 do_trace_off(void)
{
	kutrace_tracing = false;
	return kutrace_tracing;
}

/* Turn on tracing. We can only get here if all is set up */
/* Trace buffer must be allocated and initialized */
/* Return tracing bit */
static u64 do_trace_on(void)
{
	kutrace_tracing = true;
	return kutrace_tracing;
}

/* Flush all partially-filled trace blocks, filling them up */
/* Tracing must be off */
/* Return number of words zeroed */
static u64 do_flush(void)
{
	u64 *p;
	int cpu;
	int zeroed = 0;

	kutrace_tracing = false;	/* Should already be off */
	for_each_online_cpu(cpu)
	{
		struct kutrace_traceblock *tb =
			&per_cpu(kutrace_traceblock_per_cpu, cpu);
		u64 *next_item = (u64 *)atomic64_read(&tb->next);
		u64 *limit_item = tb->limit;

		if (next_item == NULL)
			continue;
		if (limit_item == NULL)
			continue;
		for (p = next_item; p < limit_item; ++p)
		{
			*p = 0;
			++zeroed;
		}

		atomic64_set(&tb->next, (u64)limit_item);
	}
	return zeroed;
}


/* Return number of filled trace blocks */
/* Next can overshoot limit when we are full */
/* Tracing will usually be on */
/* NOTE: difference of two u64* values is 1/8 of what you might be thinking */
static u64 do_stat(void)
{
	if (did_wrap_around || (traceblock_next < traceblock_limit))
		return (u64)(traceblock_high -
				traceblock_limit) >> KUTRACEBLOCKSHIFTU64;
	else
		return (u64)(traceblock_high -
				traceblock_next) >> KUTRACEBLOCKSHIFTU64;
}

/* Return number of filled trace words */
/* Tracing must be off and flush must have been called */
/* NOTE: difference of two u64* values is 1/8 of what you might be thinking */
static u64 get_count(void)
{
	u64 retval;

	kutrace_tracing = false;
	if (did_wrap_around || (traceblock_next < traceblock_limit))
		retval = (u64)(traceblock_high - traceblock_limit);
	else
		retval = (u64)(traceblock_high - traceblock_next);

	return retval;
}

/* Read and return one u64 word of trace data, working down from top.
 * This is called 1M times to dump 1M trace words (8MB), but it is called
 * by a user program that is writing all this to disk, thus is constrained
 * by disk I/O speed. So we don't care that this is somewhat inefficient
 *
 *  traceblock_limit          traceblock_next                traceblock_high
 *  |                               |                                |
 *  v                               v                                v
 *  +-------+-------+------+--------+-------+-------+-------+-------+
 *  | / / / / / / / / / / / / / / / |   3       2       1       0   |
 *  +-------+-------+------+--------+-------+-------+-------+-------+
 *                                  <==== allocated blocks grow down
 */


/* Read and return one u64 word of trace data, working down from top.
 * This is called 1M times to dump 1M trace words (8MB), but it is called
 * by a user program that is writing all this to disk, so is constrained
 * by disk I/O speed. So we don't care that this is somewhat inefficient
 */
/* Tracing must be off and flush must have been called */
static u64 get_word(u64 subscr)
{
	u64 blocknum, u64_within_block;
	u64 *blockp;

	kutrace_tracing = false;	/* Should already be off */
	if (subscr >= get_count()) return 0;
	blocknum = subscr >> KUTRACEBLOCKSHIFTU64;
	u64_within_block = subscr & ((1 << KUTRACEBLOCKSHIFTU64) - 1);
	blockp = traceblock_high - ((blocknum + 1) << KUTRACEBLOCKSHIFTU64);
	return blockp[u64_within_block];
}

/* Read and return one u64 word of IPC data, working down from top.
 *
 * Trace memory layout with IPC tracing. IPC bytes go into lower 1/8.
 *  tracebase
 *  |    traceblock_limit     traceblock_next                traceblock_high
 *  |       |                       |                                |
 *  v       v                       v                                v
 *  +-------+-------+------+--------+-------+-------+-------+-------+
 *  |////|  | / / / / / / / / / / / |                               |
 *  +-------+-------+------+--------+-------+-------+-------+-------+
 *       <==                        <==== allocated blocks grow down
 *       IPC bytes
 */

/* Tracing must be off and flush must have been called */
/* We map linear IPCword numbers 0..get_count-1 to IPC block and offset, */
/* with blocks growing downward. If mains trace blocks are 64KB, */
/* IPC blocks are 8KB */
/* Even though they are byte entries, we read them out as u64's */
static u64 get_ipc_word(u64 subscr)
{
	u64 blocknum, u64_within_block;
	u64 *blockp;

	kutrace_tracing = false;
	/* IPC word count is 1/8 of main trace count */
	if (subscr >= (get_count() >> 3))
		return 0;
	blocknum = subscr >> KUIPCBLOCKSHIFTU8;
	u64_within_block = subscr & ((1 << KUIPCBLOCKSHIFTU8) - 1);
	/* IPC blocks count down from traceblock_limit */
	blockp = traceblock_limit - ((blocknum + 1) << KUIPCBLOCKSHIFTU8);
	return blockp[u64_within_block];
}



/* We are called with preempt disabled */
/* We are called with interrupts disabled */
/* We are called holding the lock that guards traceblock_next */
/* Return first real entry slot */
static u64 *initialize_trace_block(u64 *init_me, bool very_first_block,
	struct kutrace_traceblock *tb)
{
	u64 *myclaim = NULL;
	u64 cpu = smp_processor_id();
	bool first_block_per_cpu = (tb->prior_cycles == 0);

	/* For every traceblock, insert current process ID and name. This */
	/* gives the proper context when wraparound is enabled */
	struct task_struct *curr = current;

	/* First word is rdtsc with CPU# placed in top byte */
	init_me[0] = (ku_get_cycles() & FULL_TIMESTAMP_MASK) |
		(cpu << CPU_NUMBER_SHIFT);

	/* Second word is going to be corresponding gettimeofday(), */
	/* filled in via postprocessing */
	/* We put some flags in the top byte, though. x080 = do_ipc bit */
	init_me[1] = 0;
	if (do_ipc)
		init_me[1] |= (IPC_Flag << FLAGS_SHIFT);
	if (do_wrap)
		init_me[1] |= (WRAP_Flag << FLAGS_SHIFT);
	/* We don't know if we actually wrapped until the end. */
	/* See KUTRACE_CMD_GETCOUNT */

	/* For very first trace block, also insert six NOPs at [2..7]. */
	/* The dump to disk code will overwrite the first pair with */
	/* start timepair and the next with stop timepair. [6..7] unused */
	if (very_first_block) {
		init_me[2] = 0l;
		init_me[3] = 0l;
		init_me[4] = 0l;
		init_me[5] = 0l;
		init_me[6] = 0l;
		init_me[7] = 0l;
		myclaim = &init_me[8];
	} else {
		myclaim = &init_me[2];
	}

	/* Every block has PID and pidname at the front */
	/* This requires a change for V3 in postprocessing */
	/* I feel like I should burn one more word here to make 4, */
	/* so entire front is 12/6 entries instead of 11/5... */
	myclaim[0] = curr->pid;
	myclaim[1] = 0;
	memcpy(&myclaim[2], curr->comm, MAX_PIDNAME_LENGTH);
	myclaim += 4;
	/* Next len words are the claimed space for an entry */

	/* Last 8 words of a block set to NOPs (0) */
	init_me[KUTRACEBLOCKSIZEU64 - 8] = 0;
	init_me[KUTRACEBLOCKSIZEU64 - 7] = 0;
	init_me[KUTRACEBLOCKSIZEU64 - 6] = 0;
	init_me[KUTRACEBLOCKSIZEU64 - 5] = 0;
	init_me[KUTRACEBLOCKSIZEU64 - 4] = 0;
	init_me[KUTRACEBLOCKSIZEU64 - 3] = 0;
	init_me[KUTRACEBLOCKSIZEU64 - 2] = 0;
	init_me[KUTRACEBLOCKSIZEU64 - 1] = 0;

	/* IPC design */
#if defined(__x86_64__)
	/* If this is the very first traceblock for this CPU, set up the MSRs for IPC counting */
	/* If there are 12 CPU cores (6 physical 2x hyperthreaded) this will happen 12 times */
	if (do_ipc && first_block_per_cpu) {
		setup_get_inst_retired();
		tb->prior_cycles = 1;	/* mark it as initialized */
	}
#endif

	return myclaim;
}

/* We are called with preempt disabled */
/* We are called with interrupts disabled */
/* We are called holding the lock that guards traceblock_next */
static u64 *really_get_slow_claim(int len, struct kutrace_traceblock *tb)
{
	u64 *myclaim = NULL;
	bool very_first_block = (traceblock_next == traceblock_high);

	/* Allocate a new traceblock. Allocations grow downward. */
	traceblock_next -= KUTRACEBLOCKSIZEU64;

	if (traceblock_next < traceblock_limit) {
		if (do_wrap) {
			/* Wrap to traceblock[1], not [0] */
			did_wrap_around = true;
			traceblock_next = traceblock_high -
				2 * KUTRACEBLOCKSIZEU64;
			/* Clear pid filter. */
			/* It is unfortunate to do this while holding a */
			/* lock and also holding off interrupts... */
			memset(kutrace_pid_filter, 0, 1024 * sizeof(u64));
		} else {
			/* All full. Stop and get out. */
			kutrace_tracing = false;
			return myclaim;
		}
	}

	/* Need to do this before setting next/limit if same CPU could get */
	/* an interrupt and use uninitilized block */
	/* It is unfortunate to do this while holding a lock and also */
	/* holding off interrupts... */
	/* Most of the cost is two cache misses, so maybe 200 nsec */
	myclaim = initialize_trace_block(traceblock_next, very_first_block, tb);

	/* Set up the next traceblock pointers, reserving */
	/* first N + len words */
	atomic64_set(&tb->next, (u64)(myclaim + len));
	tb->limit = traceblock_next + KUTRACEBLOCKSIZEU64;
	return myclaim;
}

/* Reserve space for one entry of 1..8 u64 words */
/* If trace buffer is full, return NULL or wrap around */
/* We allow this to be used with tracing off so we can initialize a */
/* trace file */
/* In that case, tb->next and tb->limit are NULL */
/* We are called with preempt disabled */
static u64 *get_slow_claim(int len, struct kutrace_traceblock *tb)
{
	unsigned long flags;
	u64 *limit_item;
	u64 *myclaim = NULL;

	if (is_bad_len(len)) {
		kutrace_tracing = false;
		return NULL;
	}

	/* This gets the lock that protects traceblock_next and */
	/* disables interrupts */
	raw_spin_lock_irqsave(&kutrace_lock, flags);
	/* Nothing else can be touching tb->limit now */
	limit_item = tb->limit;
	/* add_return returns the updated pointer; we want the prior */
	/* so subtract len */
	myclaim = ((u64 *)atomic64_add_return(len * sizeof(u64),
						&tb->next)) - len;
	if ((myclaim >= limit_item) || (limit_item == NULL)) {
		/* Normal case: */
		/* the claim we got still doesn't fit in its block */
		myclaim = really_get_slow_claim(len, tb);
	}
	/* Rare: If some interrupt already allocated a new traceblock, */
	/* fallthru to here */
	/* Free lock; re-enable interrupts if they were enabled on entry */
	raw_spin_unlock_irqrestore(&kutrace_lock, flags);

	return myclaim;
}

/* Reserve space for one entry of 1..8 u64 words, normally lockless */
/* If trace buffer is full, return NULL. Caller MUST check */
/* We allow this to be used with tracing off so we can initialize a */
/* trace file */
static u64 *get_claim(int len)
{
	struct kutrace_traceblock *tb;
	u64 *limit_item = NULL;
	u64 *limit_item_again = NULL;
	u64 *myclaim = NULL;

	if (is_bad_len(len)) {
		kutrace_tracing = false;
		return NULL;
	}

	/* Fast path */
	/* We may get interrupted at any point here and the interrupt routine
	 * may create a trace entry, and it may even allocate a new
	 * traceblock.
	 * This code must carefully either reserve an exclusive area to use or
	 * must call the slow path.
	 */

	/* Note that next and limit may both be NULL at initial use. */
	/* If they are, take the slow path without accessing. */

	/* get_cpu_var disables preempt ************************************/
	tb = &get_cpu_var(kutrace_traceblock_per_cpu);
	do {
		limit_item = tb->limit;
		if (limit_item == NULL)
			break;
		/* add_return returns the updated pointer; we want the */
		/* prior so subtract len */
		myclaim = ((u64 *)atomic64_add_return(len * sizeof(u64),
							&tb->next)) - len;
		limit_item_again = tb->limit;

		if (limit_item == limit_item_again)
			break;	/* All is good */
		/* An interrupt occurred *and* changed blocks */
		if ((myclaim < limit_item_again) &&
			((limit_item_again - KUTRACEBLOCKSIZEU64) <= myclaim))
			/* Claim is in new block -- use it */
			break;
		/* Else claim is at end of old block -- abandon it, */
		/* try again */
	} while (true);

	if (myclaim >= limit_item_again) {
		/* The claim we got doesn't fit in its block */
		myclaim = get_slow_claim(len, tb);
	}
	put_cpu_var(kutrace_traceblock_per_cpu);
	/* put_cpu_var re-enables preempt **********************************/

	return myclaim;
}



/* Return prior trace word for this CPU or NULL */
static u64 *get_prior(void)
{
	struct kutrace_traceblock *tb;
	u64 *next_item;
	u64 *limit_item;

	/* Note that next and limit may both be NULL at initial use. */
	/* If they are, or any other problem, return NULL */
	/* get_cpu_var disables preempt */
	tb = &get_cpu_var(kutrace_traceblock_per_cpu);
	next_item = (u64 *)atomic64_read(&tb->next);
	limit_item = tb->limit;
	put_cpu_var(kutrace_traceblock_per_cpu);

	if (next_item < limit_item)
		return next_item - 1;	/* ptr to prior entry */
	return NULL;
}


/*
 *  arg1: (arrives with timestamp = 0x00000)
 *  +-------------------+-----------+---------------+-------+-------+
 *  | timestamp         | event     | delta | retval|      arg0     |
 *  +-------------------+-----------+---------------+-------+-------+
 *           20              12         8       8           16
 */



/* Insert one u64 trace entry, for current CPU */
/* Tracing may be otherwise off    */
/* Return number of words inserted */
static u64 insert_1(u64 arg1)
{
	u64 *claim;
	u64 now = ku_get_cycles();

	claim = get_claim(1);
	if (claim != NULL) {
		claim[0] = arg1 | (now << TIMESTAMP_SHIFT);
#if defined(__x86_64__)
		/* IPC option. Changes CPU overhead from ~1/4% to ~3/4% */
		if (do_ipc) {
			struct kutrace_traceblock* tb;	/* IPC, access to prior values for this CPU */
			u64 inst_ret;
			u64 delta_cycles;
			u64 delta_inst;
			u8* ipc_byte_addr;

			/* There will be random large differences the first time; we don't care. */
			tb = &get_cpu_var(kutrace_traceblock_per_cpu);	/* hold off preempt */
			delta_cycles = now - tb->prior_cycles;
			tb->prior_cycles = now;

			inst_ret = get_inst_retired();
			delta_inst = inst_ret - tb->prior_inst_retired;
			tb->prior_inst_retired = inst_ret;
			put_cpu_var(kutrace_traceblock_per_cpu);		/* release preempt */

			/* NOTE: pointer arithmetic divides claim-base by 8, giving the byte offset we want */
			ipc_byte_addr = (u8*)(tracebase) + (claim - (u64*)(tracebase));
			ipc_byte_addr[0] = get_granular(delta_inst, delta_cycles);
		}
#endif
		return 1;
	}
	return 0;
}

/* Insert one u64 Return trace entry with small retval, for current CPU */
/* Optimize by combining with just-previous entry if the matching call */
/* and delta_t fits. The optimization is likely, so we don't worry about */
/* the overhead if we can't optimize */
/* Tracing may be otherwise off    */
/* Return number of words inserted */
static u64 insert_1_retopt(u64 arg1)
{
	u64 now = ku_get_cycles();
	u64 *prior_entry = get_prior();

	if (prior_entry != NULL) {
		/* Want N=matching call, high two bytes of arg = 0 */
		u64 diff = (*prior_entry ^ arg1) & EVENT_DELTA_RETVAL_MASK;
		u64 prior_t = *prior_entry >> TIMESTAMP_SHIFT;
		u64 delta_t = (now - prior_t) & UNSHIFTED_TIMESTAMP_MASK;

		/* make nonzero to flag there is an opt ret */
		if (delta_t == 0)
			delta_t = 1;
		/* EVENT_RETURN_BIT distinguishes call from return */
		if ((diff == EVENT_RETURN_BIT) &&
			(delta_t <= MAX_DELTA_VALUE))
		{
			/* Combine */
			u64 opt_ret;

			opt_ret = (delta_t << DELTA_SHIFT) |
				((arg1 & UNSHIFTED_RETVAL_MASK) <<
				RETVAL_SHIFT);
			*prior_entry |= opt_ret;
#if defined(__x86_64__)
			/* IPC option. Changes CPU overhead from ~1/4% to ~3/4% */
			if (do_ipc) {
				struct kutrace_traceblock* tb;	/* IPC, access to prior values for this CPU */
				u64 inst_ret;
				u64 delta_cycles;
				u64 delta_inst;
				u8* ipc_byte_addr;

				/* There will be random large differences the first time; we don't care. */
				tb = &get_cpu_var(kutrace_traceblock_per_cpu);	/* hold off preempt */
				delta_cycles = now - tb->prior_cycles;
				tb->prior_cycles = now;

				inst_ret = get_inst_retired();
				delta_inst = inst_ret - tb->prior_inst_retired;
				tb->prior_inst_retired = inst_ret;
				put_cpu_var(kutrace_traceblock_per_cpu);		/* release preempt */	

				/* NOTE: pointer arithmetic divides claim-base by 8, giving the byte offset we want */
				ipc_byte_addr = (u8*)(tracebase) + (prior_entry - (u64*)(tracebase));
				/* IPC for entry..return goes into high 4 bits of IPC byte */
				ipc_byte_addr[0] |= (get_granular(delta_inst, delta_cycles) << 4);
			}
#endif

			return 0;
		}
	}

	/* Otherwise, fall into normal insert_1 */
	return insert_1(arg1);
}


/* Insert one u64 trace entry, for current CPU */
/* Tracing may be otherwise off    */
/* Return number of words inserted */
static u64 insert_2(u64 arg1, u64 arg2)
{
	u64 now = ku_get_cycles();
	u64 *claim = get_claim(2);

	if (claim != NULL)
	{
		claim[0] = arg1 | (now << TIMESTAMP_SHIFT);
		claim[1] = arg2;
		return 2;
	}
	return 0;
}

/* For event codes 010..1FF, length is middle hex digit. All others 1 */
static u64 entry_len(u64 word)
{
	u64 n = (word >> EVENT_SHIFT) & UNSHIFTED_EVENT_MASK;

	if (n > MAX_EVENT_WITH_LENGTH)
		return 1;
	if (n < MIN_EVENT_WITH_LENGTH)
		return 1;
	return (n >> EVENT_LENGTH_FIELD_SHIFT) & EVENT_LENGTH_FIELD_MASK;
}


/* Insert one trace entry of 1..8 u64 words, for current CPU */
/* word is actually a const u64* pointer to kernel space array of */
/* exactly len u64 */
/* Tracing may be otherwise off    */
/* Return number of words inserted */
static u64 insert_n_krnl(u64 word)
{
	const u64 *krnlptr = (const u64 *)word;
	u64 len;
	u64 now;
	u64 *claim;

	len = entry_len(krnlptr[0]);	/* length in u64, 1..8 */

	/* Turn off tracing if bogus length */
	if (is_bad_len(len)) {
		kutrace_tracing = false;
		return 0;
	}

	now = ku_get_cycles();
	claim = get_claim(len);
	if (claim != NULL) {
		claim[0] = krnlptr[0] | (now << TIMESTAMP_SHIFT);
		memcpy(&claim[1], &krnlptr[1], (len - 1) * sizeof(u64));
		return len;
	}
	return 0;
}

/* Insert one trace entry of 1..8 u64 words, for current CPU */
/* word is actually a const u64* pointer to user space array of exactly */
/* eight u64 */
/* Tracing may be otherwise off */
/* Return number of words inserted */
static u64 insert_n_user(u64 word)
{
	const u64 *userptr = (const u64 *)word;
	u64 temp[8];
	u64 len;
	u64 now;
	u64 *claim;
	u64 uncopied_bytes;

	/* This call may sleep or otherwise context switch */
	/* It may fail if passed a bad user-space pointer. Don't do that. */
	temp[0] = 0;
	uncopied_bytes = raw_copy_from_user(temp, userptr, 8 * sizeof(u64));
	if (uncopied_bytes > 0)
		return 0;

	len = entry_len(temp[0]);	/* length in u64, 1..8 */

	/* Turn off tracing if bogus length */
	if (is_bad_len(len)) {
		kutrace_tracing = false;
		return 0;
	}

	now = ku_get_cycles();
	claim = get_claim(len);
	if (claim != NULL) {
		temp[0] |= (now << TIMESTAMP_SHIFT);
		memcpy(claim, temp, len * sizeof(u64));
		return len;
	}
	return 0;
}

/*
 * pid filter is an array of 64K bits, arranged as 1024 u64. It
 * cleared. When tracing context switches in kernel/sched/core.c, the
 * intended use is to check if the bit corresponding to next->pid & 0xffff is
 * off and if so put the process name next->comm[TASK_COMM_LEN]; from
 * task_struct into the trace as a pid_name entry, then set the bit.
 */

/* Reset tracing state to start a new clean trace */
/* Tracing must be off. tracebase must be non-NULL */
/* traceblock_next always points *just above* the next block to use */
/* When empty, traceblock_next == traceblock_high */
/* when full, traceblock_next == traceblock_limit */
/* Return 0 */
static u64 do_reset(u64 flags)
{
	int cpu;

	printk(KERN_INFO "kutrace_trace reset(%llu) called\n", flags);
	/* Turn off tracing -- should already be off */
	kutrace_tracing = false;	/* Should already be off */
	do_ipc = ((flags & DO_IPC) != 0);
	do_wrap = ((flags & DO_WRAP) != 0);

	/* Clear pid filter */
	memset(kutrace_pid_filter, 0, 1024 * sizeof(u64));

	/* Set up trace buffer into a series of blocks of 64KB each */
	traceblock_high = (u64 *)(tracebase + (tracemb << 20));
	traceblock_limit = (u64 *)(tracebase);
	/* First trace item inserted will cause first new block */
	traceblock_next = traceblock_high;	
	did_wrap_around = false;

	if (do_ipc) {
		/* Reserve lower 1/8 of trace buffer for IPC bytes */
		/* Strictly speaking, this should be 1/9. We waste a little space. */
		traceblock_limit = (u64*)(tracebase + (tracemb << (20 - 3)));
	}

	/* Set up spinlock as available */
	raw_spin_lock_init(&kutrace_lock);

	/* Set up per-CPU limits to immediately allocate a block */
	for_each_online_cpu(cpu) {
		struct kutrace_traceblock *tb =
			&per_cpu(kutrace_traceblock_per_cpu, cpu);

		atomic64_set(&tb->next, (u64)NULL);
		tb->limit = NULL;
	}

	return 0;
}


/* Called from kernel patches */
/* Caller is responsible for making sure event fits in 12 bits and */
/*  arg fits in 16 bits for syscall/ret and 32 bits otherwise */
static /*asmlinkage*/ void trace_1(u64 event, u64 arg)
{
        if (!kutrace_tracing)
		return;
	
	/* Check for possible return optimization */
	if (((event & UNSHIFTED_EVENT_RETURN_BIT) != 0) &&
		((event & UNSHIFTED_EVENT_HAS_RETURN_MASK) != 0))
	{
		/* We have a return entry 011x, 101x, 111x: 6/7, a/b, e/f */
		if (((arg + 128l) & ~UNSHIFTED_RETVAL_MASK) == 0) {
			/* Signed retval fits into a byte, [-128..127] */
			insert_1_retopt((event << EVENT_SHIFT) | arg);
			return;
		}
	}

	/* Non-optimized insert */
	insert_1((event << EVENT_SHIFT) | arg);
}

/* Called from kernel patches */
static void trace_2(u64 event, u64 arg1, u64 arg2)
{
	if (!kutrace_tracing)
		return;
	insert_2((event << EVENT_SHIFT) | arg1, arg2);
}

/* Called from kernel patches */
static void trace_many(u64 event, u64 len, const char *arg)
{
	u64 temp[8];

	if (!kutrace_tracing)
		return;
	/* Turn off tracing if bogus length */
	if (is_bad_len(len)) {
		kutrace_tracing = false;
		return;
	}
	memcpy(temp, arg, len * sizeof(u64));
	temp[0] |= (event | (len << EVENT_LENGTH_FIELD_SHIFT)) << EVENT_SHIFT;
	insert_n_krnl((u64)&temp[0]);
}


/* Syscall from user space via common.c kernel patch */
static u64 kutrace_control(u64 command, u64 arg)
{
	if (tracebase == NULL) {
		/* Error! */
		printk(KERN_INFO "  kutrace_control called with no trace buffer.\n");
		kutrace_tracing = false;
		return ~0l;
	}

	if (command == KUTRACE_CMD_OFF) {
		return do_trace_off();
	} else if (command == KUTRACE_CMD_ON) {
		return do_trace_on();
	} else if (command == KUTRACE_CMD_FLUSH) {
		return do_flush();
	} else if (command == KUTRACE_CMD_RESET) {
		return do_reset(arg);
	} else if (command == KUTRACE_CMD_STAT) {
		return do_stat();
	} else if (command == KUTRACE_CMD_GETCOUNT) {
		if (did_wrap_around) {
			/* Convey that we actually wrapped */
			return ~get_count();	
		} else {
			return get_count();
		}
	} else if (command == KUTRACE_CMD_GETWORD) {
		return get_word(arg);
	} else if (command == KUTRACE_CMD_GETIPCWORD) {
		return get_ipc_word(arg);
	} else if (command == KUTRACE_CMD_INSERT1) {
		return insert_1(arg);
	} else if (command == KUTRACE_CMD_INSERTN) {
		return insert_n_user(arg);
	} else if (command == KUTRACE_CMD_TEST) {
		return kutrace_tracing;	/* Just 0/1 for tracing off/on */
	} else if (command == KUTRACE_CMD_VERSION) {
		return kModuleVersionNumber;
	} else if (command == ~KUTRACE_CMD_INSERT1) {
		/* Allow kutrace_control to insert entries with tracing off */
		return insert_1(arg);
	} else if (command == ~KUTRACE_CMD_INSERTN) {
		/* Allow kutrace_control to insert entries with tracing off */
		return insert_n_user(arg);
	}

	/* Else quietly return -1 */
	return ~0l;
}


/*
 * For the compiled-into-the-kernel design, call this at first
 * kutrace_control call to set up trace buffers, etc.
 */
static int __init kutrace_mod_init(void)
{
	printk(KERN_INFO "\nkutrace_trace hello =====================\n");
	kutrace_tracing = false;

	kutrace_pid_filter = (u64 *)vmalloc(1024 * sizeof(u64));
	printk(KERN_INFO "  vmalloc kutrace_pid_filter %016lx\n",
		(long int)kutrace_pid_filter);
	if (!kutrace_pid_filter)
		return -1;

	tracebase =  vmalloc(tracemb << 20);
	printk(KERN_INFO "  vmalloc kutrace_tracebase(%ld MB) %016lx %s\n",
		tracemb,
		(long int)tracebase,
		(tracebase == NULL) ? "FAIL" : "OK");
	if (!tracebase) {
		vfree(kutrace_pid_filter);
		return -1;
	}

	/* Set up global tracing data state */
	do_reset(0);
	printk(KERN_INFO "  kutrace_tracing = %d\n", kutrace_tracing);

	/* Finally, connect up the routines that can change the state */
	kutrace_global_ops.kutrace_trace_1 = &trace_1;
	kutrace_global_ops.kutrace_trace_2 = &trace_2;
	kutrace_global_ops.kutrace_trace_many = &trace_many;
	kutrace_global_ops.kutrace_trace_control = &kutrace_control;

	printk(KERN_INFO "  kutrace_trace All done init successfully!\n");
	return 0;
}

static void __exit kutrace_mod_exit(void)
{
	int cpu;
	printk(KERN_INFO "kutrace_mod Winding down =====================\n");
	/* Turn off tracing and quiesce */
	kutrace_tracing = false;
	msleep(20);	/* wait 20 msec for any pending tracing to finish */
	printk(KERN_INFO "  kutrace_tracing=false\n");
	
	/* Disconnect allthe routiens that can change state */
	kutrace_global_ops.kutrace_trace_1 = NULL;
	kutrace_global_ops.kutrace_trace_2 = NULL;
	kutrace_global_ops.kutrace_trace_many = NULL;
	kutrace_global_ops.kutrace_trace_control = NULL;
	printk(KERN_INFO "  kutrace_global_ops = NULL\n");

	/* Clear out all the pointers to trace data */
	for_each_online_cpu(cpu) {
		struct kutrace_traceblock* tb = &per_cpu(kutrace_traceblock_per_cpu, cpu);
		printk(KERN_INFO "  kutrace_traceblock_per_cpu[%d] = NULL\n", cpu);
		atomic64_set(&tb->next, (u64)NULL);
		tb->limit = NULL;
		tb->prior_cycles = 0;		// IPC design
		tb->prior_inst_retired = 0;	// IPC design
	}

	traceblock_high = NULL;
	traceblock_limit = NULL;
	traceblock_next = NULL;  	
	
	/* Now that nothing points to it, free memory */	
	if (tracebase) {vfree(tracebase);}
	if (kutrace_pid_filter) {vfree(kutrace_pid_filter);}
	kutrace_pid_filter = NULL;

	printk(KERN_INFO "  kutrace_tracebase = NULL\n");
	printk(KERN_INFO "  kutrace_pid_filter = NULL\n");
	
	printk(KERN_INFO "kutrace_trace_ipc_mod Goodbye\n");
}


module_init(kutrace_mod_init);
module_exit(kutrace_mod_exit);


