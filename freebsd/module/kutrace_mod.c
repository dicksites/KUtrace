 /*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
 * Copyright (C) 2022 Richard L. Sites <dick.sites@gmail.com>.
 *  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice(s), this list of conditions and the following disclaimer as
 *    the first lines of this file unmodified other than the possible
 *    addition of one or more copyright notices.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice(s), this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER(S) ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDER(S) BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

/*
 * A module that implements kernel/user tracing
 *   Originally by dsites 2019.02.19
 *   FreeBSD port originally by Drew Gallatin 2019.12
 *
 * See kernel include file kutrace.h for struct definitions
 *
 * Environment variables used (ints)
 *   kutrace_mb		How much memory to reserve for the trace buffer, in MB.
 *   kutrace_nocheck	Skip privilege check if nonzero
 * 			Normally, caller of kutrace_control must have PRIV_KMEM_READ,
 *			but that is inconvenient for classroom use on a dedicated
 *			machine, so we allow skipping the check.
 *   use $ sudo kenv kutrace_xx=N to set these
 *
 * Most kernel patches will be something like
 *   kutrace1(event, arg);
 *
 *  Redesign 2022.05.25 dsites to allow non-periodic timer interrupts
 *  Redesign 2022.07.02 dsites to make FreeBSD x86 only
 *
 * This is common code for AMD and Intel x86 processors.
 * There are three pieces of information that this module uses that are potentially
 * different between hardware for the two vendors:
 *
 *   1. A time counter, rdtsc >> 6 for both
 *   2. Instructions-retired, different performance counter and different setup
 *   3. Current CPU frequency, different methods
 *
 * In the module initialization code, we use cpu_vendor to distinguish.
 *
 */


/* Crappy design. These includes cannot be in alphabetical order */
#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/proc.h>
#include <sys/kthread.h>
#include <sys/limits.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/sysproto.h>
#include <sys/sysent.h>
#include <sys/smp.h>
#include <sys/systm.h>
#include <sys/priv.h>		/* for priv_check */
#include <x86/x86_var.h>	/* for cpu_vendor */

#include <sys/kutrace.h>

/* kutrace_control() commands */
#define KUTRACE_CMD_OFF 0
#define KUTRACE_CMD_ON 1
#define KUTRACE_CMD_FLUSH 2
#define KUTRACE_CMD_RESET 3
#define KUTRACE_CMD_STAT 4
#define KUTRACE_CMD_GETCOUNT 5
#define KUTRACE_CMD_GETWORD 6
#define KUTRACE_CMD_INSERT1 7
#define KUTRACE_CMD_INSERTN 8
#define KUTRACE_CMD_GETIPCWORD 9
#define KUTRACE_CMD_TEST 10
#define KUTRACE_CMD_VERSION 11

// Added 2022.05.24 to capture high timestamp bits
#define KUTRACE_TSDELTA		0x21D	/* Delta to advance timestamp at 20-bit wrap */



/* AMD-specific defines           */
/*--------------------------------*/
/* From Open-Source Register Reference For AMD Family 17h Processors */
/*   Models 00h-2Fh */

/* IRPerfCount counts instructions retired, once set up */
/* MSR C000_00E9 [Instructions Retired Performance Count] (Core::X86::Msr::IRPerfCount) */
#define RYZEN_IRPerfCount	0xC00000E9

/* MSR C001_0015 [Hardware Configuration] (Core::X86::Msr::HWCR) */
#define RYZEN_HWCR 		0xC0010015
/*   30 IRPerfEn: enable instructions retired counter. Core::X86::Msr::IRPerfCount. */
#define RYZEN_IRPerfEn		(1L << 30)

/* AMD CPU frequency is a function of the current power state */
/* MSR C001_0063 [P-state Status] (Core::X86::Msr::PStateStat) */
/*  2:0 CurPstate: current P-state */
#define RYZEN_PStateStat 	0xC0010063
#define RYZEN_CurPstate_Shift	0
#define RYZEN_CurPstate_Mask	0x07LU

/* The eight P-states index eight MSRs that contain frequency info */
/* MSR C001_006[4...B] [P-state [7:0]] (Core::X86::Msr::PStateDef) */
/*  13:8 CpuDfsId: core divisor ID. Specifies the core frequency divisor; see CpuFid. */
/*   7:0 CpuFid[7:0]: core frequency ID. Specifies the core frequency multiplier. */
#define RYZEN_PStateDef		0xC0010064	/* First of eight MSRs */
#define RYZEN_CpuDfsId_Shift	8
#define RYZEN_CpuDfsId_Mask	0x3FLU	/* frequency divisor in increments of 1/8 */
#define RYZEN_CpuFid_Shift	0
#define RYZEN_CpuFid_Mask	0xFFLU	/* frequency in increments of 25 */
/* I think all this boils down to freq = Fid * 200 / Did */
#define RYZEN_BCLK_FREQ		200LU	/* Ryzen base clock, 25 MHz * 8 */

/* Intel-specific defines         */
/*--------------------------------*/
/* From Intel® 64 and IA-32 Architectures Software Developer’s Manual */
/*   Volume 4: Model-Specific Registers */

/* IA32_FIXED_CTR0 counts instructions retired, once set up */
#define IA32_FIXED_CTR0		0x309

/* IA32_FIXED_CTR_CTRL enables fixed counter 0 */
#define IA32_FIXED_CTR_CTRL	0x38D
#define IA32_EN0_OS		(1L << 0)	/* Count kernel events */
#define IA32_EN0_Usr		(1L << 1)	/* Count user events */
#define IA32_EN0_Anythread	(1L << 2)	/* Merge hyperthread counts */
#define IA32_EN0_PMI		(1L << 3)	/* Enable ovfl interrupt */
#define IA32_EN0_ALL_MASK	(IA32_EN0_OS | IA32_EN0_Usr | \
				 IA32_EN0_Anythread | IA32_EN0_PMI)
#define IA32_EN0_SET_MASK	(IA32_EN0_OS | IA32_EN0_Usr)	/* no merge, no irq */

/* IA32_PERF_GLOBAL_CTRL enables fixed counters */
#define IA32_PERF_GLOBAL_CTRL	0x38F
#define IA32_EN_FIXED_CTR0		(1L << 32)	/* Enable fixed counter 0 */

/* IA32_PERF_STATUS<15:8> gives current CPU frequency in increments of 100 MHz */
#define IA32_PERF_STATUS	0x198
#define IA32_FID_SHIFT		8
#define IA32_FID_MASK		0xFFL

#define IA32_BCLK_FREQ 100LU	/* CPU Intel base clock, 100 MHz */

/* Per-cpu struct */
struct kutrace_traceblock {
       uint64_t next;  /* Next uint64_t in current pcpu trace block */
       uint64_t *limit;        /* Off-the-end uint64_t in current pcpu block */
       uint64_t prior_cycles;  /* IPC tracking */
       uint64_t prior_inst_retired;    /* IPC tracking */
};



MALLOC_DEFINE(M_KUTRACE, "kutrace", "kutrace buffers");
typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;

/* Forward declarations */
static u64 kutrace_control(u64 command, u64 arg);
static int kutrace_mod_init(void);

/* For the flags byte in traceblock[1] */
#define IPC_Flag 0x80ul
#define WRAP_Flag 0x40ul

/* Incoming arg to do_reset  */
#define DO_IPC 1
#define DO_WRAP 2

/* Version number of this kernel tracing code */
static const u64 kModuleVersionNumber = 3;

/* Default values */
static const int default_tracemb = 20;	/* 20MB default */
static const int default_nocheck = 0;	/* Normally check privs */


/* IPC Inctructions per cycle flag */
static bool do_ipc;	/* Initially false */

/* Wraparound tracing vs. stop when buffer is full */
static bool do_wrap;	/* Initially false */

/* Number of MB to allocate for the trace buffer */
static long int trace_bytes;

/* Do privilege checking if true */
bool docheck;

/* Variables used to access chip-specific MSRs */
void (*setup_per_cpu_msrs)(void);	/* one of two routines, amd/intel */
u64 (*ku_get_cpu_freq)(void);		/* one of two routines, amd/intel */
int inst_retired_msr;			/* one of two MSR numbers amd/intel */


static int hackcount = 0;	/* debugging */


/* These four are exported by our patched kernel. 
 * See kernel kutrace.c
 */
extern bool kutrace_tracing;
extern struct kutrace_ops kutrace_global_ops;
extern u64* kutrace_pid_filter;
DPCPU_DEFINE(struct kutrace_traceblock, kutrace_traceblock_per_cpu);

/*
 * Individual trace entries are at least one u64, with this format:
 *
 *  +-------------------+-----------+-------+-------+-------+-------+
 *  | timestamp         | event     | delta | retval|      arg0     |
 *  +-------------------+-----------+-------+-------+-------+-------+
 *           20              12         8       8           16
 *
 * timestamp: low 20 bits of some free-running time counter in the
 *   10-40 MHz range. 
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

#define ARG_MASK       0x00000000ffffffffl
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

struct mtx kutrace_lock;

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

/*------------------------------------------------------------------------------*/
/*  Code that is x86 processor-specific (AMD or Intel)				*/
/*------------------------------------------------------------------------------*/

/*
 * Machine-specific setup and access of counters for
 *   Time counter
 *   Instructions retired
 *   CPU clock frequency
 */

/* RDMSR Read a 64-bit value from a MSR. */
/* The A constraint stands for concatenation of registers EAX and EDX. */
inline u64 rdMSR(u32 msr) {
   u32 lo, hi;
   __asm __volatile( "rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr) );
   return ((u64)lo) | (((u64)hi) << 32);
}

/* WRMSR Write a 64-bit value to a MSR. */
/* The A constraint stands for concatenation of registers EAX and EDX. */
inline void wrMSR(u32 msr, u64 value)
{
  u32 lo = value;
  u32 hi = value >> 32;
  __asm __volatile( "wrmsr" : : "a"(lo), "d"(hi), "c"(msr) );
}


/* 
 * Chips from both manufacturers need some per-CPU-core setup for reading inst_ret and 
 * cpu_freq. One of these is called once per CPU core at the beginning of tracing.
 */
static void setup_per_cpu_msrs_amd(void) {
	/* Enable instructions retired counting on current CPU */
	u64 inst_ret_enable;
	inst_ret_enable = rdMSR(RYZEN_HWCR);
	inst_ret_enable |= RYZEN_IRPerfEn;
	wrMSR(RYZEN_HWCR, inst_ret_enable);
	/*printf("  kutrace_mod rdMSR(RYZEN_HWCR) = %016lx\n", inst_ret_enable);*/
	
	/* MSR RYZEN_IRPerfCount counts instructions retired now */
}

static void setup_per_cpu_msrs_intel(void) {
	/* Enable instructions retired counting on current CPU */
	u64 inst_ret_ctrl;
	u64 inst_ret_enable;
	
	/* Configure fixed inst_ret counter 0 in IA32_FIXED_CTR_CTRL */
	/*   count both kernel and user, per-CPU-thread, no interrupt */
	inst_ret_ctrl = rdMSR(IA32_FIXED_CTR_CTRL);
	inst_ret_ctrl &= ~IA32_EN0_ALL_MASK;
	inst_ret_ctrl |=  IA32_EN0_SET_MASK;
	wrMSR(IA32_FIXED_CTR_CTRL, inst_ret_ctrl);
	/*printf("kutrace_mod IA32_FIXED_CTR_CTRL = %016lx\n", inst_ret_ctrl);*/

	/* Enable fixed inst_ret counter in IA32_PERF_GLOBAL_CTRL */
	inst_ret_enable = rdMSR(IA32_PERF_GLOBAL_CTRL);
	inst_ret_enable |= IA32_EN_FIXED_CTR0;
	wrMSR(IA32_PERF_GLOBAL_CTRL, inst_ret_enable);
	/*printf("kutrace_mod IA32_PERF_GLOBAL_CTRL = %016lx\n", inst_ret_enable);*/
	
	/* MSR IA32_FIXED_CTR0 counts instructions retired now */
}


/* Read time counter                                                          */
/* This is performance critical -- every trace entry                          */
/* Ideally, this counts at a constant rate of 16-32 nsec per count.           */
/* x86-64 version returns constant rdtsc() >> 6 to give ~20ns resolution      */
/*----------------------------------------------------------------------------*/
inline u64 ku_get_timecount(void)
{
	u64 timer_value;
	timer_value = rdtsc() >> 6;		/* Both AMD and Intel */
	return timer_value;
}

/* Read instructions retired counter                                          */
/* This is performance critical -- every trace entry if tracking IPC          */
/*----------------------------------------------------------------------------*/
inline u64 ku_get_inst_retired(void)
{
	u32 a = 0, d = 0;
	int ecx = inst_retired_msr;		/* Which counter it selects */
	 __asm __volatile("rdmsr" : "=a"(a), "=d"(d) : "c"(ecx));
	return ((u64)a) | (((u64)d) << 32);
}

/* Read current CPU frequency */
/* Not performance critical -- once every timer interrupt                     */
/*----------------------------------------------------------------------------*/
static u64 ku_get_cpu_freq_amd(void) {
	/* Sample the CPU clock frequency */
	u64 curr_pstate = (rdMSR(RYZEN_PStateStat) >> RYZEN_CurPstate_Shift) & 
		RYZEN_CurPstate_Mask;
	u64 temp = rdMSR(RYZEN_PStateDef + curr_pstate);
	u64 did = (temp >> RYZEN_CpuDfsId_Shift) & RYZEN_CpuDfsId_Mask;
	u64 fid = (temp >> RYZEN_CpuFid_Shift) & RYZEN_CpuFid_Mask;
        u64 freq = (fid * RYZEN_BCLK_FREQ) / did;
	return freq;
}

static u64 ku_get_cpu_freq_intel(void) {
	/* Sample the CPU clock frequency */
	u64 freq = rdMSR(IA32_PERF_STATUS);
        freq = (freq >> IA32_FID_SHIFT) & IA32_FID_MASK;
        freq *= IA32_BCLK_FREQ;		/* base clock in MHz */
	return freq;
}


/*------------------------------------------------------------------------------*/
/*  Code that is common to both AMD x86 and Intel x86				*/
/*------------------------------------------------------------------------------*/

/* Make sure name length fits in 1..8 u64's */
/* Return true if out of range */
inline bool is_bad_len(int len)
{
	return (len < 1) | (len > 8);
}

/* Make sure name length fits in 1 + 1..8 u64's */
/* Return true if out of range */
inline bool is_bad_len_plus(int len)
{
	return (len < 1) | (len > 9);
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
	CPU_FOREACH(cpu)
	{
		struct kutrace_traceblock* tb =
			DPCPU_ID_PTR(cpu, kutrace_traceblock_per_cpu);
		u64 *next_item = (u64 *)atomic_load_64(&tb->next);
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

		atomic_store_64(&tb->next, (u64)limit_item);
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
/* Cannot do printf or anything else here that coud block */

/* Return first real entry slot */
static u64 *initialize_trace_block(u64 *init_me, bool very_first_block,
	struct kutrace_traceblock *tb)
{
	u64 *myclaim = NULL;
	u64 cpu = curcpu;
	bool first_block_per_cpu = (tb->prior_cycles == 0);
	u64 now = ku_get_timecount();
	struct thread *curr = curthread;
	u64 freq = (*ku_get_cpu_freq)();
	
	/* Set the prior cycles to non-zero */
	if (first_block_per_cpu) {
		tb->prior_cycles = now;
	}

	/* First word is time counter with CPU# placed in top byte */
	init_me[0] = (now & FULL_TIMESTAMP_MASK) |
		(cpu << CPU_NUMBER_SHIFT);

	/* Second word is going to be corresponding gettimeofday(), */
	/* filled in via postprocessing */
	/* We put some flags in the top byte, though. 0x80 = do_ipc bit */
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

	/* For every traceblock, insert current process ID and name. This */
	/* gives the proper context when wraparound is enabled */
	/* I feel like I should burn one more word here to make 4, */
	/* so entire front is 12/6 entries instead of 11/5... */
	/*   word[0] Current PID/TID in low half and current freq in high half */
	/*   word[1] unused */
	/*   word[2] process name 16 bytes exactly */
	/*   word[3] process name 16 bytes exactly */
	myclaim[0] = curr->td_tid | (freq << 32);	
	myclaim[1] = 0;				// Unused space
	memcpy(&myclaim[2], curr->td_name, MAX_PIDNAME_LENGTH);
	myclaim += 4;
	/* Return next len words as the claimed space for an entry */

	/* Last 8 words of a block set to NOPs (0) */
	/* This initialization is needed when an entry of >1 word won't fit at the end */
	init_me[KUTRACEBLOCKSIZEU64 - 8] = 0;
	init_me[KUTRACEBLOCKSIZEU64 - 7] = 0;
	init_me[KUTRACEBLOCKSIZEU64 - 6] = 0;
	init_me[KUTRACEBLOCKSIZEU64 - 5] = 0;
	init_me[KUTRACEBLOCKSIZEU64 - 4] = 0;
	init_me[KUTRACEBLOCKSIZEU64 - 3] = 0;
	init_me[KUTRACEBLOCKSIZEU64 - 2] = 0;
	init_me[KUTRACEBLOCKSIZEU64 - 1] = 0;

	/* IPC design */
	/* If this is the very first traceblock for this CPU, set up inst_ret counting */
	/* If there are 12 CPU cores (6 physical 2x hyperthreaded) it happens 12 times */
	/* only ipc uses instructions retired */
	if (do_ipc && first_block_per_cpu) {
		setup_per_cpu_msrs();
	}

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
	atomic_store_64(&tb->next, (u64)(myclaim + len));
	tb->limit = traceblock_next + KUTRACEBLOCKSIZEU64;
	return myclaim;
}

/* Reserve space for one entry of 1..9 u64 words */
/* If trace buffer is full, return NULL or wrap around */
/* We allow this to be used with tracing off so we can initialize a trace file */
/* In that case, tb->next and tb->limit are NULL */
/* We are called with preempt disabled */
static u64 *get_slow_claim(int len, struct kutrace_traceblock *tb)
{
	u64 *limit_item;
	u64 *myclaim = NULL;

	/* This gets the lock that protects traceblock_next and */
	/* disables interrupts */
	mtx_lock_spin(&kutrace_lock);
	/* Nothing else can be touching tb->limit now */
	limit_item = tb->limit;
	myclaim = (u64 *)atomic_fetchadd_64(&tb->next, len * sizeof(u64));
	if ((myclaim >= limit_item) || (limit_item == NULL)) {
		/* Normal case: */
		/* the claim we got still doesn't fit in its block */
		myclaim = really_get_slow_claim(len, tb);
	}
	/* Rare: If some interrupt already allocated a new traceblock, */
	/* fallthru to here */
	/* Free lock; re-enable interrupts if they were enabled on entry */
	mtx_unlock_spin(&kutrace_lock);

	return myclaim;
}

/* Reserve space for one entry of 1..9 u64 words, normally lockless */
/* If trace buffer is full, return NULL. Caller MUST check */
/* We allow this to be used with tracing off so we can initialize a trace file */
/* We are called with preempt disabled */
static u64 *get_claim(int len, struct kutrace_traceblock* tb)
{
	u64 *limit_item = NULL;
	u64 *limit_item_again = NULL;
	u64 *myclaim = NULL;

	if (is_bad_len_plus(len)) {
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
	do {
		limit_item = tb->limit;
		if (limit_item == NULL)
			break;
		myclaim = (u64 *)atomic_fetchadd_64(&tb->next, len * sizeof(u64));
		limit_item_again = tb->limit;

		if (limit_item == limit_item_again)
			break;	/* All is good */
		/* An interrupt occurred *and* changed blocks */
		if ((myclaim < limit_item_again) &&
			((limit_item_again - KUTRACEBLOCKSIZEU64) <= myclaim))
			/* Claim is in new block -- use it */
			break;
		/* Else claim is at end of old block -- abandon it, and try again */
	} while (true);

	/* Make sure the entire allocation fits */
	if ((myclaim + len) >= limit_item_again) {
	    /* Either this is the first claim for a CPU */
	    /*   with limit_item, limit_item_again, and myclaim all null, or */
		/* the claim we got doesn't fit in its block. Allocate a new block. */
		myclaim = get_slow_claim(len, tb);
	}
	return myclaim;
}

/* Get a claim. If delta_cycles is large, claim one more word and insert TSDELTA entry */
/* NOTE: tsdelta is bogus for very first entry per CPU. */
/*       First per CPU is indicated by tb->prior_cycles == 0 */
/* We are called with preempt disabled */
inline u64* get_claim_with_tsdelta(u64 now, u64 delta_cycles,  
                                   int len, struct kutrace_traceblock* tb) {
	u64 *claim;
	/* Check if time between events wraps above the 20-bit timestamp */
	if (((delta_cycles & ~UNSHIFTED_TIMESTAMP_MASK) != 0) && (tb->prior_cycles != 0)) {
		/* Uncommon case. Add timestamp delta entry before original entry */
		claim = get_claim(1 + len, tb);
		if (claim != NULL) {
			claim[0] = (now << TIMESTAMP_SHIFT) | 
			           ((u64)KUTRACE_TSDELTA << EVENT_SHIFT) | (delta_cycles & ARG_MASK);
			++claim;				/* Start of space for original entry */
		}
	} else {
		/* Common case */
		claim = get_claim(len, tb);	/* Start of space for original entry */
	}
	return claim;
}

/* Return prior trace word for this CPU or NULL */
/* We are called with preempt disabled */
inline static u64 *get_prior(struct kutrace_traceblock *tb)
{	
	u64 *next_item;
	u64 *limit_item;

	/* Note that next and limit may both be NULL at initial use. */
	/* If they are, or any other problem, return NULL */
	next_item = (u64 *)atomic_load_64(&tb->next);
	limit_item = tb->limit;

	if (next_item < limit_item)
		return next_item - 1;	/* ptr to prior entry */
	return NULL;
}


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

/* Calculate and insert four-bit IPC value. Shift puts in lo/hi part of a byte */
static inline void do_ipc_calc(u64 *claim, u64 delta_cycles, 
                        struct kutrace_traceblock* tb, bool shift) {
        u64 inst_ret;
	u64 delta_inst;
	u64 ipc;
	u8* ipc_byte_addr;
	if (!do_ipc) {return;}
	/* There will be random large differences the first time; we don't care. */
	inst_ret = ku_get_inst_retired();
	delta_inst = inst_ret - tb->prior_inst_retired;
	tb->prior_inst_retired = inst_ret;
	/* NOTE: pointer arithmetic divides claim by 8, giving the byte offset we want */
	ipc_byte_addr = (u8*)(tracebase) + (claim - (u64*)(tracebase));
	ipc = get_granular(delta_inst, delta_cycles);
	if (shift)
		ipc_byte_addr[0] |= (ipc << 4);
	else
		ipc_byte_addr[0] = ipc;
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
	struct kutrace_traceblock* tb;
	u64 delta_cycles;
	u64 retval = 0;
	u64 now = ku_get_timecount();
	
	critical_enter();		/* hold off preempt */
	tb = DPCPU_PTR(kutrace_traceblock_per_cpu);
	delta_cycles = now - tb->prior_cycles;	
	/* Allocate one word */
	claim = get_claim_with_tsdelta(now, delta_cycles, 1, tb);
	/* This update must be after the first getclaim per CPU */
	tb->prior_cycles = now;
	if (claim != NULL) {
		claim[0] = arg1 | (now << TIMESTAMP_SHIFT);
		/* IPC option. Changes CPU overhead from ~1/4% to ~3/4% */
		do_ipc_calc(claim, delta_cycles, tb, false);
		retval = 1;
	}
	critical_exit();		/* release preempt */
	return retval;
}

/* Insert one u64 Return trace entry with small retval, for current CPU */
/* Optimize by combining with just-previous entry if the matching call */
/* and delta_t fits. The optimization is likely, so we don't worry about */
/* the overhead if we can't optimize */
/* Tracing may be otherwise off    */
/* Return number of words inserted */
static u64 insert_1_retopt(u64 arg1)
{
	struct kutrace_traceblock* tb;
	u64 now = ku_get_timecount();
	
	/* No need to hold off preempt here */
	/* It doesn't matter if we get migrated because we are not allocating a new entry */
	tb = DPCPU_PTR(kutrace_traceblock_per_cpu);
	u64 *prior_entry = get_prior(tb);
	if (prior_entry != NULL) {
		/* Want N=matching call, high bytes of return value = 0 */
		u64 diff = (*prior_entry ^ arg1) & EVENT_DELTA_RETVAL_MASK;
		u64 prior_t = *prior_entry >> TIMESTAMP_SHIFT;
		u64 delta_t = (now - prior_t) & UNSHIFTED_TIMESTAMP_MASK;
		/* EVENT_RETURN_BIT distinguishes call from return */
		if ((diff == EVENT_RETURN_BIT) && (delta_t <= MAX_DELTA_VALUE))
		{
			/* Successful optimization tests. Combine ret with call. */
			/* This happens about 90-95% of the time */
			u64 opt_ret;
			/* make sure delta_t is nonzero to flag there is an optimized ret */
			if (delta_t == 0)
				delta_t = 1;
			opt_ret = (delta_t << DELTA_SHIFT) |
				((arg1 & UNSHIFTED_RETVAL_MASK) << RETVAL_SHIFT);
			*prior_entry |= opt_ret;
			
			/* IPC option. Changes CPU overhead from ~1/4% to ~3/4% */
			do_ipc_calc(prior_entry, delta_t, tb, true);	
			return 0;
		}
	} 

	/* Otherwise, fall into normal insert_1 */
	return insert_1(arg1);
}


/* Insert a two-word u64 trace entry, for current CPU */
/* The entry is exactly a PC_TEMP sample */
/* Tracing may be otherwise off    */
/* Return number of words inserted */
static u64 insert_2(u64 arg1, u64 arg2)
{
	u64 *claim;
	struct kutrace_traceblock* tb;
	u64 delta_cycles;
	u64 now = ku_get_timecount();
	
	critical_enter();		/* hold off preempt */
	tb = DPCPU_PTR(kutrace_traceblock_per_cpu);
	delta_cycles = now - tb->prior_cycles;
	/* Allocate two words */
	claim = get_claim_with_tsdelta(now, delta_cycles, 2, tb);
	/* This update must be after the first getclaim per CPU */
	tb->prior_cycles = now;
	critical_exit();		/* release preempt */

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
/* Tracing may be otherwise off */
/* Return number of words inserted */
static u64 insert_n_krnl(u64 word)
{
	const u64 *krnlptr = (const u64 *)word;
	u64 len = entry_len(krnlptr[0]);	/* length in u64, 1..8 */
	u64 *claim;
	struct kutrace_traceblock* tb;
	u64 delta_cycles;
	u64 now = ku_get_timecount();

	critical_enter();		/* hold off preempt */
	tb = DPCPU_PTR(kutrace_traceblock_per_cpu);
	delta_cycles = now - tb->prior_cycles;
	/* Allocate N words */
	claim = get_claim_with_tsdelta(now, delta_cycles, len, tb);
	/* This update must be after the first getclaim per CPU */
	tb->prior_cycles = now;
	critical_exit();		/* release preempt */

	if (claim != NULL) {
		claim[0] = krnlptr[0] | (now << TIMESTAMP_SHIFT);
		memcpy(&claim[1], &krnlptr[1], (len - 1) * sizeof(u64));
		return len;
	}
	return 0;
}

/* Insert one trace entry of 1..8 u64 words, for current CPU */
/* word is actually a const u64* pointer to user space array of */
/* exactly eight u64 */
/* NOTE: Always copies eight words, even if actual length is smaller */
/* Tracing may be otherwise off */
/* Return number of words inserted */
static u64 insert_n_user(u64 word)
{
	const u64 *userptr = (const u64 *)word;
	u64 len;
	u64 *claim;
	struct kutrace_traceblock* tb;
	u64 delta_cycles;
	u64 now;
	int err;
	u64 temp[8];

	/* This call may sleep or otherwise context switch */
	/* It may fail if passed a bad user-space pointer. Don't do that. */
	temp[0] = 0;
	err = copyin(userptr, temp, 8 * sizeof(u64));
	if (err != 0)
		return 0;

	len = entry_len(temp[0]);	/* length in u64, 1..8 */
	now = ku_get_timecount();
	
	critical_enter();		/* hold off preempt */
	tb = DPCPU_PTR(kutrace_traceblock_per_cpu);
	delta_cycles = now - tb->prior_cycles;
	/* Allocate N words */
	claim = get_claim_with_tsdelta(now, delta_cycles, len, tb);
	/* This update must be after the first getclaim per CPU */
	tb->prior_cycles = now;
	critical_exit();		/* release preempt */	
	
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

	kutrace_tracing = false;	/* Should already be off */
	printf("kutrace_trace reset(%lu) called\n", flags);
	hackcount = 0;
	
	/* Turn off tracing -- should already be off */
	do_ipc = ((flags & DO_IPC) != 0);
	do_wrap = ((flags & DO_WRAP) != 0);

	/* Clear pid filter */
	memset(kutrace_pid_filter, 0, 1024 * sizeof(u64));

	/* Set up trace buffer into a series of blocks of 64KB each */
	traceblock_high = (u64 *)(tracebase + (trace_bytes));
	traceblock_limit = (u64 *)(tracebase);
	/* First trace item inserted will cause first new block */
	traceblock_next = traceblock_high;
	did_wrap_around = false;

	if (do_ipc) {
		/* Reserve lower 1/8 of trace buffer for IPC bytes */
		/* Strictly speaking, this should be 1/9. We waste a little space. */
		traceblock_limit = (u64*)(tracebase + (trace_bytes >> 3));
	}

	/* Set up per-CPU limits to immediately allocate a block */
	CPU_FOREACH(cpu) {
		struct kutrace_traceblock* tb = DPCPU_ID_PTR(cpu, kutrace_traceblock_per_cpu);
		atomic_set_64(&tb->next, (u64)NULL);
		tb->limit = NULL;
		tb->prior_cycles = 0;		// IPC design
		tb->prior_inst_retired = 0;	// IPC design
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
		
//if ((event == 0x607) && (hackcount++ < 50)) {
//printf("trace_1 %lx %lx \n", event, arg);
//}

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
/* ONLY called to insert PC sample at timer interrupt */
/* arg1 is unused (0), arg2 is the 64-bit PC sample */
static void trace_2(u64 event, u64 arg1, u64 arg2)
{
	u64 freq;
	if (!kutrace_tracing)
		return;
	/* dsites 2021.04.05 also insert CPU frequency as arg0 */
	freq = (*ku_get_cpu_freq)();
	insert_2((event << EVENT_SHIFT) | freq, arg2);
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
static uint64_t
kutrace_control(uint64_t command, uint64_t arg)
{
	if (docheck) {
	        int error = priv_check(curthread, PRIV_KMEM_READ);
        	if (error != 0)
			return ~0l;	/* not allowed */
	}

	if (tracebase == NULL) {
		/* Error! */
		kutrace_tracing = false;
		printf("  ERROR kutrace_control called with no trace buffer.\n");
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
                /* If not tracing, insert nothing */
                if (!kutrace_tracing)
                        return 0;
                return insert_1(arg);
        } else if (command == KUTRACE_CMD_INSERTN) {
                /* If not tracing, insert nothing */
                if (!kutrace_tracing)
                        return 0;
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
 * For the compiled-into-the-kernel design, call this at load time
 * Uses environment variables kutrace_mb and kutrace_nocheck
 */
static int kutrace_mod_init(void)
{
	const size_t pid_size = 1024 * sizeof(u64);
	int param_int;
	kutrace_tracing = false;
	printf("\nkutrace_trace init =====================\n");
	
	/* Pick vendor-specific values, AMD/Intel */
	if (strcmp(cpu_vendor, "AuthenticAMD") == 0) {
		setup_per_cpu_msrs = &setup_per_cpu_msrs_amd;
		ku_get_cpu_freq =    &ku_get_cpu_freq_amd;
		inst_retired_msr =   RYZEN_IRPerfCount;	/*  MSR counts instr retired */
	} else if (strcmp(cpu_vendor, "GenuineIntel") == 0) {
		setup_per_cpu_msrs = &setup_per_cpu_msrs_intel;
		ku_get_cpu_freq =    &ku_get_cpu_freq_intel;
		inst_retired_msr =   IA32_FIXED_CTR0;	/*  MSR counts instr retired */
	} else {
		printf("  FAIL: cannot initialize for vendor %s\n", cpu_vendor);
		return -1;
	}
	
	/* Allocate 8KB for the pid name filter (64K bits) */
	kutrace_pid_filter = (u64 *)malloc(pid_size, M_KUTRACE, M_WAITOK);
	if (!kutrace_pid_filter) {
		return -1;
	}

	/* Allocate the trace buffer */
	param_int = 0;
	getenv_int("kutrace_mb", &param_int);
	if (param_int <= 0) {param_int = default_tracemb;}
	trace_bytes = param_int;
	trace_bytes <<= 20;	/* Allowed to be bigger than 2GB */
	tracebase =  malloc(trace_bytes, M_KUTRACE, M_ZERO);
	printf("  kutrace_tracebase(%ld MB) %016lx %s\n",
		trace_bytes >> 20,
		(long int)tracebase,
		(tracebase == NULL) ? "FAIL" : "OK");
	if (!tracebase) {
		free(kutrace_pid_filter, M_KUTRACE);
		return -1;
	}
	
	/* Remember whether to check privileges */
	param_int = 0;
	getenv_int("kutrace_nocheck", &param_int);
	printf("  kutrace nocheck=%d\n", param_int);
	if (param_int <= 0) {param_int = default_nocheck;}
	docheck = (param_int == 0);
	printf("  kutrace using privilege check: %s\n", docheck ? "YES" : "NO");

	/* Set up spinlock as available */
	mtx_init(&kutrace_lock, "kutrace lock", "kutrace", MTX_SPIN);

	/* Set up global tracing data state */
	do_reset(0);
	printf("  kutrace_tracing = %d\n", kutrace_tracing);

	/* Finally, connect up the routines that can change the state */
	kutrace_global_ops.kutrace_trace_1 = &trace_1;
	kutrace_global_ops.kutrace_trace_2 = &trace_2;
	kutrace_global_ops.kutrace_trace_many = &trace_many;
	kutrace_global_ops.kutrace_trace_control = &kutrace_control;


	printf("  kutrace_trace initialized successfully!\n");
	return 0;
}

static void kutrace_mod_exit(void)
{
	int cpu;
	kutrace_tracing = false;
	printf("kutrace module Winding down =====================\n");
	/* Turn off tracing and quiesce */
	/* wait 20 msec for any pending tracing to finish */
	pause_sbt("kutrace unload", SBT_1MS * 20, SBT_1MS, 0);
	printf("  kutrace_tracing=false\n");

	/* Disconnect all the routines that can change state */
	kutrace_global_ops.kutrace_trace_1 = NULL;
	kutrace_global_ops.kutrace_trace_2 = NULL;
	kutrace_global_ops.kutrace_trace_many = NULL;
	kutrace_global_ops.kutrace_trace_control = NULL;
	printf("  kutrace_global_ops = NULL\n");

	/* Clear out all the pointers to trace data */
	CPU_FOREACH(cpu) {
		struct kutrace_traceblock* tb =
			DPCPU_ID_PTR(cpu, kutrace_traceblock_per_cpu);
		printf("  kutrace_traceblock_per_cpu[%d] = NULL\n", cpu);
		atomic_set_64(&tb->next, (u64)NULL);
		tb->limit = NULL;
		tb->prior_cycles = 0;		// IPC design
		tb->prior_inst_retired = 0;	// IPC design
	}

	traceblock_high = NULL;
	traceblock_limit = NULL;
	traceblock_next = NULL;
	
	/* Now that nothing points to it, free memory */
	free(tracebase, M_KUTRACE);
	free(kutrace_pid_filter, M_KUTRACE);
	kutrace_pid_filter = NULL;
	mtx_destroy(&kutrace_lock);

	printf("  kutrace_tracebase = NULL\n");
	printf("  kutrace_pid_filter = NULL\n");

	printf("kutrace_mod Goodbye\n");
}

/*
 * The function called at load/unload.
 */

static int kutrace_syscall_num = NO_SYSCALL;


static int
load(struct module *module __unused, int cmd, void *arg __unused)
{
	int error;

	error = 0;

	switch (cmd) {
	case MOD_LOAD:
		/* initialize the subsystem */
		error = kutrace_mod_init();
		if (error != 0)
			break;
		printf("kutrace: syscall=%d\n", kutrace_syscall_num);
		break;

	case MOD_UNLOAD:
	case MOD_SHUTDOWN:
		kutrace_mod_exit();
		printf("kutrace: unload\n");
		break;

	default :
		error = EINVAL;
		break;
	}

	return (error);
}

struct kutrace_control_args {
	uint64_t	cmd;
	uint64_t	arg;
	void		*base;
};

static int
kutrace_control_sys(struct thread *td, void *arg)
{
	struct kutrace_control_args *uap = arg;
	uint64_t rval;

	rval = kutrace_control(uap->cmd, uap->arg);
	/*
	 * FreeBSD syscalls return ints, so we must
	 * copy the 64-bit retval out.
	 */

	return (suword(uap->base, rval));	/* suword64 ?? */
}

static struct sysent kutrace_sysent = {
	.sy_narg =	3,
	.sy_call =	kutrace_control_sys,
};


SYSCALL_MODULE(kutrace, &kutrace_syscall_num, &kutrace_sysent,
    load, NULL);
