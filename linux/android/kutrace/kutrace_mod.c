/*
 * kutrace_mod.c
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
 * A module that implements kernel/user tracing
 * dsites 2023.02.18
 *
 * See include/linux/kutrace.h for struct definitions
 *
 * Most patches will be something like
 *   kutrace1(event, arg) which calls trace_1 here
 *
 */


/*
 * kutrace.c -- kernel/user tracing implementation
 * dsites 2019.02.14 Reworked for the 4.19 kernel, from dclab_trace.c
 * dsites 2020.02.04 fixed getclaim(n) bug for n > 1
 * dsites 2020.10.30 Add packet trace parameters
 *  use something like
 *  sudo insmod kutrace_mod.ko tracemb=20 pktmask=0x0000000f pktmatch=0xd1c517e5
 *  default is the above
 *  pktmask=0 traces nothing, pktmask=-1 traces all (no pktmatch needed)
 * dsites 2021.09.25 Add Rpi-4B 64-bit support
 * dsites 2023.02.13 Add fast 4KB trace buffer extraction
 * dsites 2023.02.13 Change module version number to 4
 * dsites 2023.02.16 Merge in TSDELTA code from FreeBSD version
 *
 */

#include <linux/kutrace.h>

#include <linux/capability.h>
#include <linux/cpufreq.h>
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
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <asm/atomic.h>
#include <asm/uaccess.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Richard L Sites");

// Added 2023.02.13. Move these into the kernel at next build
#ifndef KUTRACE_CMD_SET4KB
#define KUTRACE_CMD_SET4KB 12
#endif

#ifndef KUTRACE_CMD_GET4KB
#define KUTRACE_CMD_GET4KB 13
#endif

#ifndef KUTRACE_CMD_GETIPC4KB
#define KUTRACE_CMD_GETIPC4KB 14
#endif

#ifndef KUTRACE_TSDELTA
#define KUTRACE_TSDELTA         0x21D  /* Delta to advance timestamp */
#endif



// GCC compiler options to distinguish build targets
// Use to get these:
//   gcc -dM -E -march=native - < /dev/null

/* Add others as you find and test them */
#if defined(__x86_64__)
#define Isx86_64        1
#else
#define Isx86_64        0
#endif

#if defined(__znver1)
#define IsAmd_64        Isx86_64
#define IsIntel_64	0
#else
#define IsAmd_64        0
#define IsIntel_64	Isx86_64
#endif

#if defined(__aarch64__)
#define IsArm_64        1
#else
#define IsArm_64        0
#endif


/* AMD-specific defines           */
/*--------------------------------*/
/* From Open-Source Register Reference For AMD Family 17h Processors */
/*   Models 00h-2Fh */

/* rdtsc counts cycles, no setup needed */

/* IRPerfCount counts instructions retired, once set up */
#define IRPerfCount		0xC00000E9
#define RYZEN_HWCR 		0xC0010015
#define IRPerfEn		(1L << 30)

/* PStateStat<2:0> gives current P-state of a core */
/* PStateDefn<13:8> Did gives frequency divisor in increments of 1/8 */
/* PStateDefn<7:0> Fid gives frequency in increments of 25 */
/* I think all this boils down to freq = Fid * 200 / Did, but it could be 266.67 */
#define PStateStat 		0xC0010063
#define PStateDef0		0xC0010064
#define PStateDef1		0xC0010065
#define PStateDef2		0xC0010066
#define PStateDef3		0xC0010067
#define PStateDef4		0xC0010068
#define PStateDef5		0xC0010069
#define PStateDef6		0xC001006A
#define PStateDef7		0xC001006B
#define PStat_MASK		0x07LU
#define CpuDid_SHIFT		8
#define CpuDid_MASK		0x3FLU
#define CpuFid_SHIFT		0
#define CpuFid_MASK		0xFFLU

// amd notes
// FIDVID_STATUS HwPstate
// MSRC001_006[4...B] [P-state [7:0]] (Core::X86::Msr::PStateDef)
// freq = <7:0> * 25 MHz * CpuDid in <13:8> VCO
// From https://developer.amd.com/wp-content/resources/56255_3_03.PDF
// sudo watch -n 1 cpupower monitor


/* Intel-specific defines         */
/*--------------------------------*/
/* From Intel® 64 and IA-32 Architectures Software Developer’s Manual */
/*   Volume 4: Model-Specific Registers */

/* rdtsc counts cycles, no setup needed */
/* IA32_FIXED_CTR0 counts instructions retired, once set up */
#define IA32_FIXED_CTR0		0x309
#define IA32_FIXED_CTR_CTRL	0x38D
#define EN0_OS			(1L << 0)
#define EN0_Usr			(1L << 1)
#define EN0_Anythread		(1L << 2)
#define EN0_PMI			(1L << 3)
#define EN0_ALL			(EN0_OS | EN0_Usr | EN0_Anythread | EN0_PMI)
#define IA32_PERF_GLOBAL_CTRL	0x38F
#define EN_FIXED_CTR0		(1L << 32)

/* MSR_IA32_PERF_STATUS<15:8> gives current CPU frequency in increments of 100 MHz */
#define MSR_PERF_STATUS		0x198
#define FID_SHIFT		8
#define FID_MASK		0xFFL


/* Arm-speficic defines           */
/*--------------------------------*/
/* Old 32-bit defines in  linux-4.19.19/arch/arm/kernel/perf_event_v6.c */

#if IsArm_64
/* This is for 64-bit ARM */
typedef long long int int64;
typedef long long unsigned int uint64;
#define FLX "%016llx"
#define FLD "%lld"
#define FUINTPTRX "%016lx"
#define CL(x) x##LL
#define CLU(x) x##LLU
#define ATOMIC_READ atomic64_read
#define ATOMIC_SET atomic64_set
#define ATOMIC_ADD_RETURN atomic64_add_return

#elif Isx86_64
/* This is for 64-bit X86 */
typedef long int int64;
typedef long unsigned int uint64;
#define FLX "%016lx"
#define FLD "%ld"
#define FUINTPTRX "%016lx"
#define CL(x) x##L
#define CLU(x) x##LU
#define ATOMIC_READ atomic64_read
#define ATOMIC_SET atomic64_set
#define ATOMIC_ADD_RETURN atomic64_add_return

#else
#error Need type defines for your architecture

#endif


#if IsAmd_64
#define BCLK_FREQ 200LU	/* CPU Ryzen base clock, assume 25 MHz * 8 */

#elif IsIntel_64
#define BCLK_FREQ 100LU	/* CPU Intel base clock, assume 100 MHz */

#else
#define BCLK_FREQ 0LU	/* CPU RPi, frequency sampling not implemented -- change notifications used */

#endif


/* Forward declarations */
static u64 kutrace_control(u64 command, u64 arg);
static int __init kutrace_mod_init(void);

/* For the flags byte in traceblock[1] */
#define IPC_Flag CLU(0x80)
#define WRAP_Flag CLU(0x40)

/* Incoming arg to do_reset  */
#define DO_IPC 1
#define DO_WRAP 2

/* Module parameter: default how many MB of kernel trace memory to reserve */
/* This is for the standalone, non-module version */
/* static const long int kTraceMB = 32; */

/* Version number of this kernel tracing code */
/* 2023.02.13 Incremented to 4 for fast 4KB trace buffer extraction */
static const u64 kModuleVersionNumber = 4;


/* A few global variables */

/* IPC Instructions per cycle flag */
static bool do_ipc;	/* Initially false */

/* Wraparound tracing vs. stop when buffer is full */
static bool do_wrap;	/* Initially false */

/* Current offset to use for fast 4KB trace buffer extraction get4kb and getipc4kb */
/* Set by KUTRACE_CMD_SET4KB call */
static u64 get4kb_subscr;	/* Initially zero */

/* Module parameter: default how many MB of kernel trace memory to reserve */
static long int tracemb = 2;
static long int check = 1;

/* Module parameters: packet filtering. Initially match just dclab RPC markers */
static long int pktmask  = 0x0000000f;
static long int pktmatch = 0xd1c517e5;

module_param(tracemb, long, S_IRUSR);
MODULE_PARM_DESC(tracemb, "MB of kernel trace memory to reserve (2)");
module_param(check, long, S_IRUSR);
MODULE_PARM_DESC(check, "0: no checking, 1: require PTRACE capability for DoControl (1)");
module_param(pktmask, long, S_IRUSR);
MODULE_PARM_DESC(pktmask, "Bit-per-byte of which bytes to use in hash");
module_param(pktmatch, long, S_IRUSR);
MODULE_PARM_DESC(pktmatch, "Matching hash value");


/* These four are exported by our patched kernel.
 * See linux-4.19.19/kernel/kutrace/kutrace.c
 */
extern bool kutrace_tracing;
extern struct kutrace_ops kutrace_global_ops;
extern u64* kutrace_pid_filter;
extern struct kutrace_nf kutrace_net_filter;
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

#define ARG_MASK       CLU(0x00000000ffffffff)
#define ARG0_MASK      CLU(0x000000000000ffff)
#define RETVAL_MASK    CLU(0x0000000000ff0000)
#define DELTA_MASK     CLU(0x00000000ff000000)
#define EVENT_MASK     CLU(0x00000fff00000000)
#define TIMESTAMP_MASK CLU(0xfffff00000000000)
#define EVENT_DELTA_RETVAL_MASK (EVENT_MASK | DELTA_MASK | RETVAL_MASK)
#define EVENT_RETURN_BIT        CLU(0x0000020000000000)
#define EVENT_LENGTH_FIELD_MASK CLU(0x000000000000000f)

#define UNSHIFTED_RETVAL_MASK CLU(0x00000000000000ff)
#define UNSHIFTED_DELTA_MASK  CLU(0x00000000000000ff)
#define UNSHIFTED_EVENT_MASK  CLU(0x0000000000000fff)
#define UNSHIFTED_TIMESTAMP_MASK   CLU(0x00000000000fffff)
#define UNSHIFTED_EVENT_RETURN_BIT CLU(0x0000000000000200)
#define UNSHIFTED_EVENT_HAS_RETURN_MASK CLU(0x0000000000000c00)

#define MIN_EVENT_WITH_LENGTH CLU(0x010)
#define MAX_EVENT_WITH_LENGTH CLU(0x1ff)
#define MAX_DELTA_VALUE 255
#define MAX_PIDNAME_LENGTH 16

#define RETVAL_SHIFT 16
#define DELTA_SHIFT 24
#define EVENT_SHIFT 32
#define TIMESTAMP_SHIFT 44
#define EVENT_LENGTH_FIELD_SHIFT 4

#define FULL_TIMESTAMP_MASK CLU(0x00ffffffffffffff)
#define CPU_NUMBER_SHIFT 56

#define GETTIMEOFDAY_MASK   CLU(0x00ffffffffffffff)
#define FLAGS_SHIFT 56

/* For deciding that large timestamp advance is really a late store */
/* with backward time. */
static const u64 kLateStoreThresh = 0x00000000000e0000LLU;


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

#if IsArm_64
  // XXX: This is specific to the Pixel 6 Pro
  /* "cycle" counter is 24MHz, cycles are 2400 MHz, so one count = 2400/24 = 100 cycles */
  /* Call it 96 (less than 1% error). To get 8*inst/cycles for the divide below, we mul by 8/96 = 1/12 */
  del_cycles = (u32)(delta_cycles * 12);   /* cycles/96 to cycles/8 */
#else
  del_cycles = (u32)(delta_cycles << 3);  /* cycles/64 to cycles/8 */
#endif

  ipc = del_inst / del_cycles;	          /* gives IPC*8 */
  return kIpcMapping[ipc & 0x3F];	  /* Truncate unexpected IPC >= 8.0 */
}


/* Machine-specific register Access utilities */
/*----------------------------------------------------------------------------*/
#if Isx86_64
/* RDMSR Read a 64-bit value from a MSR. */
/* The A constraint stands for concatenation of registers EAX and EDX. */
static inline u64 rdMSR(u32 msr) {
   u32 lo, hi;
   asm volatile( "rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr) );
   return ((u64)lo) | (((u64)hi) << 32);
}

/* WRMSR Write a 64-bit value to a MSR. */
/* The A constraint stands for concatenation of registers EAX and EDX. */
static inline void wrMSR(u32 msr, u64 value)
{
  u32 lo = value;
  u32 hi = value >> 32;
  asm volatile( "wrmsr" : : "a"(lo), "d"(hi), "c"(msr) );
}
#endif


/* Set up global state for reading time, retired, freq */
/*----------------------------------------------------------------------------*/

/* Set up global state for reading scaled CPU cycles */
/* This needs to run once on each CPU core */
/* For ARM, make sure it increments every 64 cycles, not 1 */
static void ku_setup_timecount(void)
{
#if IsArm_64
	/* No setup needed for cntvct */
#elif Isx86_64
	/* No setup needed for rdtsc */
#elif IsArm_64
	/* No setup needed; count every 1 cycle for ccnt is the default */
#else
#endif
}


/* Set up global state for reading instructions retired */
/* This needs to run once on each CPU core */
static void ku_setup_inst_retired(void)
{
#if IsAmd_64
	u64 inst_ret_enable;
	/* Enable fixed inst_ret counter  */
	inst_ret_enable = rdMSR(RYZEN_HWCR);
	printk(KERN_INFO "  kutrace_mod rdMSR(RYZEN_HWCR) = %016llx\n", inst_ret_enable);
	inst_ret_enable |= IRPerfEn;
	wrMSR(RYZEN_HWCR, inst_ret_enable);

#elif IsIntel_64
	u64 inst_ret_ctrl;
	u64 inst_ret_enable;
	/* cpuCount_HW_INSTRUCTIONS = 1<<30 */

	/* Configure fixed inst_ret counter in IA32_FIXED_CTR_CTRL */
	/*   count both kernel and user, count per-CPU-thread, no interrupt */
	inst_ret_ctrl = rdMSR(IA32_FIXED_CTR_CTRL);
	printk(KERN_INFO "  kutrace_mod rdMSR(IA32_FIXED_CTR_CTRL) = %016llx\n", inst_ret_ctrl);
	inst_ret_ctrl &= ~EN0_ALL;
	inst_ret_ctrl |=  (EN0_OS | EN0_Usr);
	wrMSR(IA32_FIXED_CTR_CTRL, inst_ret_ctrl);

	/* Enable fixed inst_ret counter in IA32_PERF_GLOBAL_CTRL */
	inst_ret_enable = rdMSR(IA32_PERF_GLOBAL_CTRL);
	printk(KERN_INFO "  kutrace_mod rdMSR(IA32_PERF_GLOBAL_CTRL) = %016llx\n", inst_ret_enable);
	inst_ret_enable |= EN_FIXED_CTR0;
	wrMSR(IA32_PERF_GLOBAL_CTRL, inst_ret_enable);

#elif IsArm_64
	/* Setup needed for instruction counting */
	/* set up pmevtyper0<15:0> to count INST_RETIRED =0x08 */
	/* set up pmcntenset<0>=1 to enable */
	u64 evtcount = 8;	/* INST_RETIRED */
	u64 r = 0;
	asm volatile("mrs %x0, pmcr_el0" : "=r" (r));
	asm volatile("msr pmcr_el0, %x0" : : "r" (r | 1));	/* enable pmu */
	asm volatile("msr pmevtyper2_el0, %x0" : : "r" (evtcount));	/* count inst_retired */
	asm volatile("mrs %x0, pmcntenset_el0" : "=r" (r));
	asm volatile("msr pmcntenset_el0, %x0" : : "r" (r|1<<2));	/* enable cntr[2] */

#else
#error Define ku_setup_inst_retired for your architecture

#endif
}

/* Set up global state for reading CPU frequency */
/* This needs to run once on each CPU core */
static void ku_setup_cpu_freq(void)
{
	/* No setup for AMD, Intel, RPi4 */
}


/*x86-64 or Arm-specific time counter, ideally 30-60 MHz (16-32 nsec) */
/* Arm64 RPi4 returns 54MHz counts, 18.52ns */
/* x86-64 version returns constant rdtsc() >> 6 to give ~20ns resolution */


/* Read a time counter */
/* This is performance critical -- every trace entry  */
/* Ideally, this counts at a constant rate of 16-32 nsec per count.           */
/*----------------------------------------------------------------------------*/
inline u64 ku_get_timecount(void)
{
	u64 timer_value;
#if IsArm_64
	asm volatile("mrs %x0, cntvct_el0" : "=r"(timer_value));
#elif Isx86_64
	/* If you change this shift amount, change it in kutrace_lib.cc also */
	timer_value = rdtsc() >> 6;		/* Both AMD and Intel */
#else
#error Define the time counter for your architecture
	timer_value = 0;
#endif

	return timer_value;
}


/* Read instructions retired counter */
/* This is performance critical -- every trace entry if tracking IPC */
/*----------------------------------------------------------------------------*/
inline u64 ku_get_inst_retired(void)
{
#if IsAmd_64
	u32 a = 0, d = 0;
	int ecx = IRPerfCount;		/* What counter it selects, AMD */
	 __asm __volatile("rdmsr" : "=a"(a), "=d"(d) : "c"(ecx));
	return ((u64)a) | (((u64)d) << 32);

#elif IsIntel_64
	u32 a = 0, d = 0;
	int ecx = IA32_FIXED_CTR0;	/* What counter it selects, Intel */
	__asm __volatile("rdmsr" : "=a"(a), "=d"(d) : "c"(ecx));
	return ((u64)a) | (((u64)d) << 32);

#elif IsArm_64
	u64 value = 0;
	/* set up pmevtyper2<15:0> to count INST_RETIRED =0x08 */
	/* set up pmcntenset<0>=1<<2 to enable */
	asm volatile("mrs %x0, pmevcntr2_el0" : "=r" (value));
	return value;

#else
#error Define inst_retired for your architecture
	return 0;

#endif

}

/* Read current CPU frequency */
/* Not performance critical -- once every timer interrupt                     */
/*----------------------------------------------------------------------------*/
inline u64 ku_get_cpu_freq(void) {
#if !BCLK_FREQ
	return 0;

#elif IsAmd_64
        /* Sample the CPU clock frequency and include with PC sample */
	u64 curr = rdMSR(PStateStat) & PStat_MASK;
        u64 freq = rdMSR(PStateDef0 + curr);
	u64 fid = (freq >> CpuFid_SHIFT) & CpuFid_MASK;
	u64 did = (freq >> CpuDid_SHIFT) & CpuDid_MASK;
        freq = (fid * BCLK_FREQ) / did;
	return freq;

#elif IsIntel_64
	u64 freq = rdMSR(MSR_PERF_STATUS);
        freq = (freq >> FID_SHIFT) & FID_MASK;
        freq *= BCLK_FREQ;	/* base clock in MHz */
	return freq;

#else
#error Define cpu_freq for your architecture
	return 0;

#endif
}

/* Return true for large time advance that should be treated as small backward time */
inline bool LateStoreOrLarge(u64 delta_cycles) {
  return delta_cycles > kLateStoreThresh;
}


/* Make sure name length fits in 1..8 u64's */
//* Return true if out of range */
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
	for_each_online_cpu(cpu)
	{
		struct kutrace_traceblock *tb =
			&per_cpu(kutrace_traceblock_per_cpu, cpu);
		u64 *next_item = (u64 *)ATOMIC_READ(&tb->next);
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

		ATOMIC_SET(&tb->next, (uintptr_t)limit_item);
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
/* printk(KERN_INFO "get_word[%lld] %016llx\n", subscr, blockp[u64_within_block]); */
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

/*
 * Fast 4KB trace buffer extraction: Copies 4KB at once to user space.
 * Because the syscall that gets here supplies only one argument, we use it as
 * the user-space target buffer address. For the kernel-space buffer offset we
 * use get4kb_subscr.
 *
 * Caller must do KUTRACE_CMD_SET4KB to initialize this
 * and then must do KUTRACE_CMD_GET4KB and optionally KUTRACE_CMD_GETIPC4KB
 * for every block of 4KB/sizeof(traceword) = 512 words.
 *
 * 4KB extraction is measured to be about 165x faster than 8B extraction per
 * 64KB traceblock. Including buffered writes toward disk/SSD, this achieves
 * about 1400MB/s vs 25MB/s overall on an Intel i3 at 3.9 GHz, so about 55
 * times faster, i.e. 55x less CPU time during extraction.
 *
 * Calls to the 4KB commands when run against an older module will return ~0.
 * It is up to the caller to detect this and use the word-at-a-time routines.
 *
 */

/* Read and return one 4KB block of trace data, working down from top,
 * then increment get4kb_subscr by 4KB/sizeof(traceword) = 512.
 * This is called 1M/512 = 2K times to dump 1M trace words (8MB).
 * Returns 0 for success, ~0 for unimplemented, and 1..4096 for partial copy.
 * Tracing must be off and flush must have been called
 */
static u64 get_4kb(u64 arg)
{
	u64 blocknum, u64_within_block;
	u64 *blockp;
	void __user *to_user_ptr;
	const void *from_kernel_ptr;

	if (get4kb_subscr >= get_count())
		return 4096;

	blocknum = get4kb_subscr >> KUTRACEBLOCKSHIFTU64;
	u64_within_block = get4kb_subscr & ((1 << KUTRACEBLOCKSHIFTU64) - 1);
	blockp = traceblock_high - ((blocknum + 1) << KUTRACEBLOCKSHIFTU64);
/* printk(KERN_INFO "get_4kb[%lld] %016llx\n", get4kb_subscr, blockp[u64_within_block]); */
	to_user_ptr = (void __user *)arg;
	from_kernel_ptr = (const void *)(&blockp[u64_within_block]);
	return copy_to_user(to_user_ptr, from_kernel_ptr, 4096);
}

/* Read and return one 4KB block of IPC data, working down from top.
 * This is called 1M/512 = 2K times to dump 1M IPC words (8MB).
 * Returns 0 for success, ~0 for unimplemented, and 1..4096 for partial copy.
 * Tracing must be off and flush must have been called
 */
static u64 get_ipc_4kb(u64 arg)
{
	u64 blocknum, u64_within_block;
	u64 *blockp;
	void __user *to_user_ptr;
	const void *from_kernel_ptr;

	/* IPC word count is 1/8 of main trace count */
	if (get4kb_subscr >= (get_count() >> 3))
		return 4096;

	blocknum = get4kb_subscr >> KUIPCBLOCKSHIFTU8;
	u64_within_block = get4kb_subscr & ((1 << KUIPCBLOCKSHIFTU8) - 1);
	/* IPC blocks count down from traceblock_limit */
	blockp = traceblock_limit - ((blocknum + 1) << KUIPCBLOCKSHIFTU8);
	to_user_ptr = (void __user *)arg;
	from_kernel_ptr = (const void *)(&blockp[u64_within_block]);
	return copy_to_user(to_user_ptr, from_kernel_ptr, 4096);
}



/* We are called with preempt disabled */
/* We are called with interrupts disabled */
/* We are called holding the lock that guards traceblock_next */
/* Cannot do printf or anything else here that could block */
static u64 *initialize_trace_block(u64 *init_me, bool very_first_block,
	struct kutrace_traceblock *tb)
{
	u64 *myclaim = NULL;
	u64 cpu = smp_processor_id();

	/* For every traceblock, insert current process ID and name. This */
	/* gives the proper context when wraparound is enabled */
	struct task_struct *curr = current;

	/* First word is rdtsc (time counter) with CPU# placed in top byte */
	u64 block_init_counter = ku_get_timecount();
	init_me[0] = (block_init_counter & FULL_TIMESTAMP_MASK) |
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
		init_me[2] = CLU(0);
		init_me[3] = CLU(0);
		init_me[4] = CLU(0);
		init_me[5] = CLU(0);
		init_me[6] = CLU(0);
		init_me[7] = CLU(0);
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

	/* If this is the very first traceblock for this CPU, set up the MSRs */
	/* If there are 12 CPU cores (6 physical 2x hyperthreaded) this will happen 12 times */
	{
		bool first_block_per_cpu = (tb->prior_cycles == 0);
		if (first_block_per_cpu) {
			ku_setup_timecount();
			ku_setup_inst_retired();
			ku_setup_cpu_freq();
			tb->prior_cycles = 1;	/* mark it as initialized */
#if IsArm_64
			{
			struct cpufreq_policy *policy = cpufreq_cpu_get_raw(cpu);
			/* For Rpi4, put current CPU freq (MHz) into block at high half of myclaim[-4] */
			if (policy) {
				u64 cpu_freq_mhz = policy->cur / 1000;	/* Khz to MHz */
				myclaim[-4] |= (cpu_freq_mhz << 32);
/*printk(KERN_INFO "cpu %lld freq = %lld MHz\n", cpu, cpu_freq_mhz);*/
			}
			}
#endif

		}
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
	ATOMIC_SET(&tb->next, (uintptr_t)(myclaim + len));
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
	unsigned long flags;
	u64 *limit_item;
	u64 *myclaim = NULL;

	if (is_bad_len(len)) {
		kutrace_tracing = false;
printk(KERN_INFO "is_bad_len 1\n");
		return NULL;
	}

	/* This gets the lock that protects traceblock_next and */
	/* disables interrupts */
	raw_spin_lock_irqsave(&kutrace_lock, flags);
	/* Nothing else can be touching tb->limit now */
	limit_item = tb->limit;
	/* add_return returns the updated pointer; we want the prior */
	/* so subtract len */
	myclaim = ((u64 *)ATOMIC_ADD_RETURN(len * sizeof(u64), &tb->next)) -
			 len;
	/* FIXED BUG: myclaim + len */
	if (((myclaim + len) >= limit_item) || (limit_item == NULL)) {
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

		/* add_return returns the updated pointer; we want the */
		/* prior so subtract len */
		myclaim =
		((u64 *)ATOMIC_ADD_RETURN(len * sizeof(u64), &tb->next)) - len;
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


/*
 * In recording a trace event, it is possible for an interrupt to happen after
 * KUtrace code takes the event timestamp and before it claims the storage location.
 * In this case, the interupt handling will recursively record several events
 * before returning to the original KUtrace path, which then claims a location
 * and stores the original event with its earlier timestamp. This is called a
 * "late store." When that happens, the reconstruciton in rawtoevent needs to
 * decide whether time went forward by almost the entire 20-bit wraparound
 * period, or went backward by some amount.
 *
 * To resolve this ambiguity, we declare that a time gap of 7/8 of the wraparound
 * period is forward time and the high 1/8 is backward time associated with an
 * otherwise undetectable backward time.
 *
 * To mark forward time in that 1/8 (and above), we add a TSDELTA entry to the
 * trace. The exact compare for late store must be identical in kutrace_mod.c
 * and in rawtoevent.cc.
 *
 */

/* Get a claim. If delta_cycles is large, claim one more word and insert TSDELTA entry */
/* NOTE: tsdelta is bogus for very first entry per CPU. */
/*       First per CPU is indicated by tb->prior_cycles == 0 */
/* We are called with preempt disabled */
inline u64* get_claim_with_tsdelta(u64 now, u64 delta_cycles,
                                   int len, struct kutrace_traceblock* tb) {
	u64 *claim;
	/* Check if time between events almost wraps above the 20-bit timestamp */
	if (LateStoreOrLarge(delta_cycles) && (tb->prior_cycles != 0)) {
		/* Uncommon case. Add timestamp delta entry before original entry */
		claim = get_claim(1 + len, tb);
		if (claim != NULL) {
			claim[0] = (now << TIMESTAMP_SHIFT) |
			           ((u64)KUTRACE_TSDELTA << EVENT_SHIFT) |
                                   (delta_cycles & ARG_MASK);
			++claim;		/* Start of space for original entry */
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
	/* get_cpu_var disables preempt */
	tb = &get_cpu_var(kutrace_traceblock_per_cpu);
	next_item = (u64 *)ATOMIC_READ(&tb->next);
	limit_item = tb->limit;
	put_cpu_var(kutrace_traceblock_per_cpu);

	if (next_item < limit_item)
		return next_item - 1;	/* ptr to prior entry */
	return NULL;
}

/* Calculate and insert four-bit IPC value. Shift puts in lo/hi part of a byte */
inline void do_ipc_calc(u64 *claim, u64 delta_cycles,
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
				ipc_byte_addr[0] |= ipc << 4;
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

	tb = &get_cpu_var(kutrace_traceblock_per_cpu);	/* hold off preempt */
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
	put_cpu_var(kutrace_traceblock_per_cpu);	/* release preempt */
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
	u64 *prior_entry;
	u64 now = ku_get_timecount();

	/* No need to hold off preempt here, but get_cpu/put_cpu do anyway */
	/* It doesn't matter if we get migrated because we are not allocating a new entry */
	tb = &get_cpu_var(kutrace_traceblock_per_cpu);	/* hold off preempt */
	prior_entry = get_prior(tb);
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
			put_cpu_var(kutrace_traceblock_per_cpu);	/* release preempt */
			return 0;
		}
	}
	put_cpu_var(kutrace_traceblock_per_cpu);	/* release preempt */

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

	tb = &get_cpu_var(kutrace_traceblock_per_cpu);	/* hold off preempt */
	delta_cycles = now - tb->prior_cycles;
	/* Allocate two words */
	claim = get_claim_with_tsdelta(now, delta_cycles, 2, tb);
	/* This update must be after the first getclaim per CPU */
	tb->prior_cycles = now;
	put_cpu_var(kutrace_traceblock_per_cpu);	/* release preempt */

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

	tb = &get_cpu_var(kutrace_traceblock_per_cpu);	/* hold off preempt */
	delta_cycles = now - tb->prior_cycles;
	/* Allocate N words */
	claim = get_claim_with_tsdelta(now, delta_cycles, len, tb);
	/* This update must be after the first getclaim per CPU */
	tb->prior_cycles = now;
	put_cpu_var(kutrace_traceblock_per_cpu);	/* release preempt */

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
	const uintptr_t tempword = word;	/* 32- or 64-bit pointer */
	const u64 *userptr = (const u64 *)tempword;
	u64 len;
	u64 *claim;
	struct kutrace_traceblock* tb;
	u64 delta_cycles;
	u64 now;
	u64 uncopied_bytes;
	u64 temp[8];

	/* This call may sleep or otherwise context switch */
	/* It may fail if passed a bad user-space pointer. Don't do that. */
	temp[0] = 0;
	uncopied_bytes = raw_copy_from_user(temp, userptr, 8 * sizeof(u64));
	if (uncopied_bytes > 0)
		return 0;

	len = entry_len(temp[0]);	/* length in u64, 1..8 */
	now = ku_get_timecount();

	tb = &get_cpu_var(kutrace_traceblock_per_cpu);	/* hold off preempt */
	delta_cycles = now - tb->prior_cycles;
	/* Allocate N words */
	claim = get_claim_with_tsdelta(now, delta_cycles, len, tb);
	/* This update must be after the first getclaim per CPU */
	tb->prior_cycles = now;
	put_cpu_var(kutrace_traceblock_per_cpu);	/* release preempt */

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

	/* printk(KERN_INFO "  kutrace_trace reset(%016llx) called\n", flags); */
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

		ATOMIC_SET(&tb->next, (uintptr_t)NULL);
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
	insert_1((event << EVENT_SHIFT) | (arg & CLU(0xffffffff)));
}

/* Called from kernel patches */
/* ONLY called to insert PC sample at timer interrupt */
/* arg1 is unused (0), arg2 is the 64-bit PC sample */
static void trace_2(u64 event, u64 arg1, u64 arg2)
{
	u64 freq;
	if (!kutrace_tracing)
		return;

/* dsites 2021.04.05 insert CPU frequency */
	freq = ku_get_cpu_freq();
	insert_2((event << EVENT_SHIFT) | freq, arg2);
}

/* Called from kernel patches */
static void trace_many(u64 event, u64 len, const char *arg)
{
	uintptr_t tempptr;	/* 32- or 64-bit address */
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
	tempptr = (uintptr_t)&temp[0];
	insert_n_krnl((u64)tempptr);
}


/* Syscall from user space via kernel patch */
static u64 kutrace_control(u64 command, u64 arg)
{
/*
 * printk(KERN_INFO "  kutrace_control: %08x %08x %08x %08x\n",
 *   (u32)(command & 0xFFFFFFFF), (u32)(command >> 32),
 *   (u32)(arg & 0xFFFFFFFF), (u32)(arg >> 32));
 */
	if (tracebase == NULL) {
		/* Error! */
		printk(KERN_INFO "  kutrace_control called with no trace buffer.\n");
		kutrace_tracing = false;
		return ~CLU(0);
	}

	/* If checking, disallow calls from tasks without CAP_SYS_PTRACE */
	// XXX: We assume that the current user has the capability to trace.
	// Android does not seem to export the has_capability function for use
	// with kernel modules
	// if (check && !has_capability(current, CAP_SYS_PTRACE))
	// 	return ~CLU(0);

	/* Generally, more likely calls are near the front of this list */
	if (command == KUTRACE_CMD_OFF) {
		return do_trace_off();
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
	} else if (command == KUTRACE_CMD_GETWORD) {
		return get_word(arg);
	} else if (command == KUTRACE_CMD_GETIPCWORD) {
		return get_ipc_word(arg);
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
	} else if (command == KUTRACE_CMD_SET4KB) {
		/* This returns 0 for success. */
		/* Older module versions will return ~0 for unknown command */
		get4kb_subscr = arg;
		return 0;
	} else if (command == KUTRACE_CMD_GET4KB) {
		return get_4kb(arg);
	} else if (command == KUTRACE_CMD_GETIPC4KB) {
		return get_ipc_4kb(arg);
	}

	/* Else quietly return -1 */
	return ~CLU(0);
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
	printk(KERN_INFO "  vmalloc kutrace_pid_filter " FUINTPTRX "\n",
		(uintptr_t)kutrace_pid_filter);
	if (!kutrace_pid_filter)
		return -1;

	tracebase =  vmalloc(tracemb << 20);
	printk(KERN_INFO "  vmalloc kutrace_tracebase(%ld MB) " FUINTPTRX " %s\n",
		tracemb,
		(uintptr_t)tracebase,
		(tracebase == NULL) ? "FAIL" : "OK");
	if (!tracebase) {
		vfree(kutrace_pid_filter);
		return -1;
	}

	/* Set up TCP packet filter */
	/* Filter forms a hash over masked first N=24 bytes of packet payload */
	/* and looks for zero result. The hash is just u32 XOR along with */
	/* an initial value. pktmask gives mask bit-per-byte, and pktmatch */
	/* gives the expected result over those bytes. It is the */
	/* inital hash value, to give a simple zero test at the end. */
	if (pktmask == 0) {
		// Match nothing
		kutrace_net_filter.hash_mask[0] = 0LLU;
		kutrace_net_filter.hash_mask[1] = 0LLU;
		kutrace_net_filter.hash_mask[2] = 0LLU;
		kutrace_net_filter.hash_init = 1;	// hash will always be zero
	} else if (pktmask == -1) {
		// Match everything
		kutrace_net_filter.hash_mask[0] = 0LLU;
		kutrace_net_filter.hash_mask[1] = 0LLU;
		kutrace_net_filter.hash_mask[2] = 0LLU;
		kutrace_net_filter.hash_init = 0;	// hash will always be zero

	} else {
		int i;
		u8 *msk = (u8*)(kutrace_net_filter.hash_mask);
		for (i = 0; i < 24; ++i) {
			if ((pktmask >> i) & 1) {msk[i] = 0xFF;}
			else {msk[i] = 0x00;}
		}
		kutrace_net_filter.hash_init = (u64)(pktmatch);
	}
	printk(KERN_INFO "  mask %016llx", kutrace_net_filter.hash_mask[0]);
	printk(KERN_INFO "  mask %016llx", kutrace_net_filter.hash_mask[1]);
	printk(KERN_INFO "  mask %016llx", kutrace_net_filter.hash_mask[2]);
	printk(KERN_INFO "   ==  %016llx", kutrace_net_filter.hash_init);

#if IsAmd_64
	printk(KERN_INFO "IsAmd_64");
#endif
#if IsIntel_64
	printk(KERN_INFO "IsIntel_64");
#endif
#if IsArm_64
	printk(KERN_INFO "IsArm_64");
#endif

	/* Set up global tracing data state */
	/* Very first traceblock alloc per CPU will do this, but we need */
	/* the timecount set up before we write teh first trace entry */
	ku_setup_timecount();
	ku_setup_inst_retired();
	ku_setup_cpu_freq();
	do_reset(0);
	printk(KERN_INFO "  kutrace_tracing = %d\n", kutrace_tracing);

	/* Finally, connect up the routines that can change the state */
	kutrace_global_ops.kutrace_trace_1 = &trace_1;
	kutrace_global_ops.kutrace_trace_2 = &trace_2;
	kutrace_global_ops.kutrace_trace_many = &trace_many;
	kutrace_global_ops.kutrace_trace_control = &kutrace_control;

	printk(KERN_INFO "  &kutrace_global_ops: " FUINTPTRX "\n", (uintptr_t)(&kutrace_global_ops));
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
		ATOMIC_SET(&tb->next, (uintptr_t)NULL);
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

	printk(KERN_INFO "kutrace__mod Goodbye\n");
}


module_init(kutrace_mod_init);
module_exit(kutrace_mod_exit);


