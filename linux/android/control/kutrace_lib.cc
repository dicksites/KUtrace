// Little user-mode library program to control kutracing
// Copyright 2023 Richard L. Sites
//

#include <stdio.h>
#include <stdlib.h>     // exit, system
#include <string.h>
#include <time.h>	// nanosleep
#include <unistd.h>     // getpid gethostname syscall
#include <sys/time.h>   // gettimeofday
#include <sys/types.h>

#if defined(__x86_64__)
#include <x86intrin.h>		// _rdtsc
#endif

#include "basetypes.h"
#include "kutrace_control_names.h"	// PidNames, TrapNames, IrqNames, Syscall64Names
#include "kutrace_lib.h"

// All the real stuff is inside this anonymous namespace
namespace {

/* Outgoing arg to DoReset  */
#define DO_IPC 1
#define DO_WRAP 2

/* For the flags byte in traceblock[1] */
#define IPC_Flag     CLU(0x80)
#define WRAP_Flag    CLU(0x40)
#define Unused2_Flag CLU(0x20)
#define Unused1_Flag CLU(0x10)
#define VERSION_MASK CLU(0x0F)


// Module/code must be at least this version number for us to run
static const u64 kMinModuleVersionNumber = 3;

// Module/code must be at least this version number for us to use fast 4KB dump
static const u64 kMin4KBModuleVersionNumber = 4;

// This defines the format of the resulting trace file
static const u64 kTracefileVersionNumber = 3;

// NOTE: To use fast 4KB transfers out of trace buffer,
//  IPC block must be at least 4KB and thus trace block must be at least 32KB.

// Number of u64 values per 4KB
static const int k4KBSize = 512;

// Number of u64 values per trace block (64KB)
static const int kTraceBufSize = 8192;

// Number of u64 values per IPC block, one u8 per u64 in trace buf
static const int kIpcBufSize = kTraceBufSize >> 3;

// For wraparound fixup on Raspberry Pi-4B Arm-v7
static const int mhz_32bit_cycles = 54;

// Globals for mapping cycles to gettimeofday
int64 start_cycles = 0;
int64 stop_cycles = 0;
int64 start_usec = 0;
int64 stop_usec = 0;

// Globals for trace context
const int kMaxBufferSize = 256;
const int GetbufSize = 64;
typedef char irqname[GetbufSize];
char kernelversion[GetbufSize];
char modelname[GetbufSize];
char hostname[GetbufSize];
char linkspeed[GetbufSize];
NumNamePair localirqpairs[256];	// At most 256 IRQ name/number pairs
irqname irqnames[256];		// At most 256 IRQ names


// Useful utility routines
int64 GetUsec() {
  struct timeval tv; gettimeofday(&tv, NULL);
  return (tv.tv_sec * CL(1000000)) + tv.tv_usec;
}

#ifdef __riscv
/* Rigamarole to read CSR_TIME register */
#define CSR_CYCLE               0xc00
#define CSR_TIME                0xc01
#define CSR_INSTRET             0xc02

#ifdef __ASSEMBLY__
#define __ASM_STR(x)    x
#else
#define __ASM_STR(x)    #x
#endif

#define csr_read(csr)                                           \
({                                                              \
        /*register*/ unsigned long __v;                             \
        __asm__ __volatile__ ("csrr %0, " __ASM_STR(csr)        \
                              : "=r" (__v) :                    \
                              : "memory");                      \
        __v;                                                    \
})
#endif


/* Counts by one for each 64 cycles or so */
/* x86-64 or Arm or Risc-v specific timer */
/* Arm-64 returns 32MHz counts: 31.25 ns each */
/* Arm-32 Raspberry Pi4B 54MHz counts: 18.52 nsec */
/* x86-64 version returns rdtsc() >> 6 to give ~20ns resolution */
inline u64 ku_get_cycles(void)
{
	u64 timer_value;
#if defined(__aarch64__)
	asm volatile("mrs %x0, cntvct_el0" : "=r"(timer_value));
#elif defined(__ARM_ARCH_ISA_ARM)
	/* This 32-bit result at 54 MHz RPi4 wraps every 75 seconds */
	asm volatile("mrrc p15, 1, %Q0, %R0, c14" : "=r" (timer_value));
	timer_value &= CLU(0x00000000FFFFFFFF);
#elif defined(__x86_64__)
	timer_value = _rdtsc() >> 6;
#elif defined(__riscv)
	/* HiFive Unmatched is 1 MHz and 400cy to read. Sigh. */
	timer_value = csr_read(CSR_TIME);
#else
	BUILD_BUG_ON_MSG(1, "Define the time base for your architecture");
#endif
	return timer_value;
}


// Read time counter and gettimeofday() close together, returning both
void GetTimePair(int64* cycles, int64* usec) {
  int64 startcy, stopcy;
  int64 gtodusec, elapsedcy;
  // Do more than once if we get an interrupt or other big delay in the middle of the loop
  do {
    startcy = ku_get_cycles();
    gtodusec = GetUsec();
    stopcy = ku_get_cycles();
    elapsedcy = stopcy - startcy;
    // In a quick test on an Intel i3 chip, GetUsec() took about 150 cycles (50 nsec)
    //  Perhaps 4x this on Arm chips
    // printf("%ld elapsed cycles\n", elapsedcy);
  } while (elapsedcy > 320);  // About 20,000 cycles if counting one for 64 cycles
  *cycles = startcy;
  *usec = gtodusec;
}


// For the trace_control system call,
// arg is declared to be u64. In reality, it is either a u64 or
// a pointer to a u64, depending on the command. Caller casts as
// needed, and the command implementations in kutrace_mod
// cast back as needed.

// These numbers must exactly match the numbers in kernel include file kutrace.h
// This maps to highest syscall32 when 0x800 is added
#define KUTRACE_SCHEDSYSCALL 1535

void StripCRLF(char* s) {
  int len = strlen(s);
  if ((0 < len) && s[len - 1] == '\n') {s[len - 1] = '\0'; --len;}
  if ((0 < len) && s[len - 1] == '\r') {s[len - 1] = '\0'; --len;}
}

//--------------------------------------------------------------------------------------//
// FreeBSD-specific routines
//--------------------------------------------------------------------------------------//

#if defined(__FreeBSD__)
// FreeBSD syscalls are different.  The nice thing is that
// we can dynamically register a system call.
//
// The not so nice thing is that we need to then look up
// the syscall number which was assigned, and the even
// less nice thing is that FreeBSD syscalls return ints,
// so we have to copy out the return value

#include <sys/types.h>
#include <sys/param.h>
#include <sys/module.h>
#include <sys/syscall.h>

static int __NR_kutrace_control = -1;
u64 inline DoControl(u64 command, u64 arg)
{
  u64 rval;
  int err;

  if (__predict_false(__NR_kutrace_control == -1)) {
    struct module_stat ms;
    int mod_id;

    mod_id = modfind("sys/kutrace");
    if (mod_id < 0) {
      return (-1);
    }
    ms.version = sizeof(ms);
    err = modstat(mod_id, &ms);
    if (err < 0) {
      return (-1);
    }
    __NR_kutrace_control = ms.data.intval;
    if (__NR_kutrace_control < 0) {
      return (-1);
    }
  }
  err = syscall(__NR_kutrace_control, command, arg, &rval);
  if (err != 0) {
    return (-1);
  }
  return (rval);
}

// Model number is in sysctl output
void GetModelName(char* modelname, int len) {
  modelname[0] = '\0';
  FILE *fp = popen("sysctl hw.model", "r");
  if (fp == NULL) {return;}
  char* s = fgets(modelname, len, fp);
  pclose(fp);
  // Expecting something like
  // hw.model: Intel(R) Core(TM) i3-7100 CPU @ 3.90GHz
  if (s != NULL) {
    char* colon = strchr(s, ':');
    if ((colon != NULL) && (colon[1] ^= '\0')) {
      // Get rid of leading "kw.model: "
      char* dst = modelname;
      char* src = colon + 2;	// Over the colon and space
      while (*src != '\0') {*dst++ = *src++;}
    }
  }
  StripCRLF(modelname);
}

// Get next interrupt description line from file, if any, and set
// interrupt number and name and return value of true.
// If no more lines, return false
//
// Expecting:
//   irq1: atkbd0                           0          0
//   intermixed with other stuff
//
// first grep picks out irq lines with an actual name
// first sed turns tabs to spaces
// second grep keeps only transformed defines
// third grep removes defines that have no number in the right place

bool NextIntr(FILE* intrfile, int* intrnum, char* intrname, int len) {
  char buffer[kMaxBufferSize];
  while (fgets(buffer, kMaxBufferSize, intrfile)) {
    StripCRLF(buffer);
    char c;
    int n = sscanf(buffer, "irq%d: %c", intrnum, &c);
    if (n != 2) {continue;}			// No intr on this line
    const char* colon =  strchr(buffer, ':');
    c = *(colon + 2);				// First letter of name, or space/tab
    if (c == ' ') {continue;}			// No intr name on this line
    if (c == '\t') {continue;}			// No intr name on this line
    const char* space = strchr(colon + 2, ' ');	// just after the name
    if (space == NULL) {continue;}		// No name on this line
    *(char*)space = '\0';
    strncpy(intrname, colon + 2, len);
    intrname[len - 1] = '\0';
    return true;
  }

  return false;
}

// Read up to 255 active IRQ names from the running system
void GetIrqNames(NumNamePair* irqpairs, irqname* irqnames) {
  irqpairs[0].number = -1;	// Default end marker
  irqpairs[0].name = NULL;
  FILE* intrfile = popen("vmstat -ia", "r");
  if (intrfile == NULL) {return;}
  char intrname[GetbufSize];
  int intrnum;
  int k = 0;
  while (NextIntr(intrfile, &intrnum, intrname, GetbufSize)) {
    memcpy(irqnames[k], intrname, GetbufSize);	// Make a copy
    irqpairs[k].number = intrnum;
    irqpairs[k].name = &irqnames[k][0];
    ++k;
    if (255 <= k) {break;}	// Leaving room for NULL end-marker
  }
  fclose(intrfile);
  irqpairs[k].number = -1;	// End marker
  irqpairs[k].name = NULL;
}

#endif

//--------------------------------------------------------------------------------------//
// Linux-specific routines
//--------------------------------------------------------------------------------------//

#if !defined(__FreeBSD__)

static const int __NR_kutrace_control = 1023;

u64 inline DoControl(u64 command, u64 arg)
{
  return syscall(__NR_kutrace_control, command, arg);
}

// Model number is in /proc/cpuinfo
void GetModelName(char* modelname, int len) {
  modelname[0] = '\0';
  FILE *cpuinfo = fopen("/proc/cpuinfo", "rb");
  if (cpuinfo == NULL) {return;}
  char *arg = NULL;
  size_t size = 0;
  // Expecting something like
  // model name	: ARMv7 Processor rev 3 (v7l)
  while(getline(&arg, &size, cpuinfo) != -1)
  {
    if(memcmp(arg, "model name", 10) == 0) {
      const char* colon = strchr(arg, ':');
      if (colon != NULL) {	// Skip the colon and the next space
        StripCRLF(arg);
        strncpy(modelname, colon + 2, len);
        modelname[len - 1] = '\0';
        break;			// Just the first one, then get out
      }
    }
  }
  free(arg);
  fclose(cpuinfo);
  StripCRLF(modelname);
}

// Get next interrupt description line from file, if any, and set
// interrupt number and name and return value of true.
// If no more lines, return false
//
// Expecting:
// cat /proc/interrupts
//            CPU0       CPU1
//   0:         20          0   IO-APIC   2-edge      timer
//   1:          3          0   IO-APIC   1-edge      i8042
//   8:          1          0   IO-APIC   8-edge      rtc0
bool NextIntr(FILE* intrfile, int* intrnum, char* intrname, int len) {
  char buffer[kMaxBufferSize];
  while (fgets(buffer, kMaxBufferSize, intrfile)) {
    StripCRLF(buffer);
    int n = sscanf(buffer, "%d:", intrnum);
    if (n != 1) {continue;}			// No intr on this line
    const char* space = strrchr(buffer, ' ');	// NOTE: reverse search
    if (space == NULL) {continue;}		// No name on this line
    if (space[1] == '\0') {continue;}		// Empty name on this line
    strncpy(intrname, space + 1, len);
    intrname[len - 1] = '\0';
    return true;
  }

  return false;
}

// Read up to 255 active IRQ names from the running system
void GetIrqNames(NumNamePair* irqpairs, irqname* irqnames) {
  irqpairs[0].number = -1;	// Default end marker
  irqpairs[0].name = NULL;
  FILE* intrfile = fopen("/proc/interrupts", "r");
  if (intrfile == NULL) {return;}

  char intrname[GetbufSize];
  int intrnum;
  int k = 0;
  while (NextIntr(intrfile, &intrnum, intrname, GetbufSize)) {
    memcpy(irqnames[k], intrname, GetbufSize);	// Make a copy
    irqpairs[k].number = intrnum;
    irqpairs[k].name = &irqnames[k][0];
    ++k;
    if (255 <= k) {break;}	// Leaving room for NULL end-marker
  }
  fclose(intrfile);
  irqpairs[k].number = -1;	// End marker
  irqpairs[k].name = NULL;
}

#endif

//--------------------------------------------------------------------------------------//
// Common routines
//--------------------------------------------------------------------------------------//

// Kernel version is the result of command: uname -rv
void GetKernelVersion(char* kernelversion, int len) {
  kernelversion[0] = '\0';
  FILE *fp = popen("uname -v", "r");
  if (fp == NULL) {return;}
  char* s = fgets(kernelversion, len, fp);
  pclose(fp);
  StripCRLF(kernelversion);
}

// Host name
void GetHostName(char* hostname, int len) {
  hostname[0] = '\0';
  gethostname(hostname, len) ;
  hostname[len - 1] = '\0';
  StripCRLF(hostname);
}

// TBD main Ethernet link speed
void GetLinkSpeed(char* linkspeed,int len) {
  linkspeed[0] = '\0';
}


#if 0
#if defined(__FreeBSD__)
// FreeBSD syscalls are different.  The nice thing is that
// we can dynamically register a system call.
//
// The not so nice thing is that we need to then look up
// the syscall number which was assigned, and the even
// less nice thing is that FreeBSD syscalls return ints,
// so we have to copy out the return value

#include <sys/types.h>
#include <sys/param.h>
#include <sys/module.h>
#include <sys/syscall.h>

static int __NR_kutrace_control = -1;
u64 inline DoControl(u64 command, u64 arg)
{
  u64 rval;
  int err;

  if (__predict_false(__NR_kutrace_control == -1)) {
    struct module_stat ms;
    int mod_id;

    mod_id = modfind("sys/kutrace");
    if (mod_id < 0) {
      return (-1);
    }
    ms.version = sizeof(ms);
    err = modstat(mod_id, &ms);
    if (err < 0) {
      return (-1);
    }
    __NR_kutrace_control = ms.data.intval;
    if (__NR_kutrace_control < 0) {
      return (-1);
    }
  }
  err = syscall(__NR_kutrace_control, command, arg, &rval);
  if (err != 0) {
    return (-1);
  }
  return (rval);
}
#else

// Not FreeBSD
// These numbers must exactly match the numbers in kernel include file kutrace.h
#define __NR_kutrace_control 1023
u64 inline DoControl(u64 command, u64 arg)
{
  return syscall(__NR_kutrace_control, command, arg);
}
#endif
#endif


// Sleep for n milliseconds
void msleep(int msec) {
  struct timespec ts;
  ts.tv_sec = msec / 1000;
  ts.tv_nsec = (msec % 1000) * 1000000;
  nanosleep(&ts, NULL);
}

// Single static buffer. In real production code, this would
// all be std::string value, or something else at least as safe.
static const int kMaxDateTimeBuffer = 32;
static char gTempDateTimeBuffer[kMaxDateTimeBuffer];

// Turn seconds since the epoch into yyyymmdd_hhmmss
// Not valid after January 19, 2038
const char* FormatSecondsDateTime(int32 sec) {
  // if (sec == 0) {return "unknown";}  // Longer spelling: caller expecting date
  time_t tt = sec;
  struct tm* t = localtime(&tt);
  sprintf(gTempDateTimeBuffer, "%04d%02d%02d_%02d%02d%02d",
         t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
         t->tm_hour, t->tm_min, t->tm_sec);
  return gTempDateTimeBuffer;
}

// Construct a name for opening a trace file, using name of program from command line
//   name: program_time_host_pid
// str should hold at least 256 bytes
const char* MakeTraceFileName(const char* argv0, char* str) {
  const char* slash = strrchr(argv0, '/');
  // Point to first char of image name
  if (slash == NULL) {
    slash = argv0;
  } else {
    slash = slash + 1;  // over the slash
  }

  const char* timestr;
  time_t tt = time(NULL);
  timestr = FormatSecondsDateTime(tt);

  char hostnamestr[GetbufSize];
  gethostname(hostnamestr, GetbufSize) ;
  hostnamestr[GetbufSize - 1] = '\0';

  int pid = getpid();

  sprintf(str, "%s_%s_%s_%d.trace", slash, timestr, hostnamestr, pid);
  return str;
}

// This depends on ~KUTRACE_CMD_INSERTN working even with tracing off.
void InsertVariableEntry(const char* str, u64 event, u64 arg) {
  u64 temp[8];		// Up to 56 bytes
  u64 bytelen = strlen(str);
  if (bytelen == 0) {return;}		// Skip empty strings
  if (bytelen > 56) {bytelen = 56;}	// If too long, truncate
  u64 wordlen = 1 + ((bytelen + 7) / 8);
  // Build the initial word
  u64 event_with_length = event + (wordlen * 16);
  //         T               N                           ARG
  temp[0] = (CLU(0) << 44) | (event_with_length << 32) | arg;
  memset(&temp[1], 0, 7 * sizeof(u64));
  memcpy((char*)&temp[1], str, bytelen);
  DoControl(~KUTRACE_CMD_INSERTN, (u64)&temp[0]);
}

// Add a list of names to the trace
void EmitNames(const NumNamePair* ipair, u64 event) {
  u64 temp[9];		// One extra word for strcpy(56 bytes + '\0')
  const NumNamePair* pair = ipair;
  while (pair->name != NULL) {
    InsertVariableEntry(pair->name, event, pair->number);
    ++pair;
  }
}



// This depends on ~TRACE_INSERTN working even with tracing off.
void InsertTimePair(int64 cycles, int64 usec) {
  u64 temp[8];		// Always 8 words for TRACE_INSERTN
  u64 n_with_length = KUTRACE_TIMEPAIR + (3 << 4);
  temp[0] = (CLU(0) << 44) | (n_with_length << 32);
  temp[1] = cycles;
  temp[2] = usec;
  DoControl(~KUTRACE_CMD_INSERTN, (u64)&temp[0]);
}



// Return false if the module is not loaded or too old. No delay. No side effect on time.
bool TestModule() {
  // If module is not loaded, syscall 511 returns -1 or -ENOSYS (= -38)
  // Unsigned, these are bigger than the biggest plausible version number, 255
  u64 retval = DoControl(KUTRACE_CMD_VERSION, 0);
// VERYTEMP
// fprintf(stderr, "TestModule %08lx %08lx\n", swi_ret0, swi_ret1);

  if (retval > 255) {
    // Module is not loaded
    fprintf(stderr, "KUtrace module/code not loaded\n");
    return false;
  }
  if (retval < kMinModuleVersionNumber) {
    // Module is loaded but older version
    fprintf(stderr, "KUtrace module/code is version %lld. Need at least %lld\n",
      retval, kMinModuleVersionNumber);
    return false;
  }
  //fprintf(stderr, "KUtrace module/code is version %ld.\n", retval);
  return true;
}


// Return true if module is loaded and tracing is on, else false
// CMD_TEST returns -ENOSYS (= -38) if not a tracing kernel
// else returns 0 if tracing is off
// else returns 1 if tracing is on
bool DoTest() {
  u64 retval = DoControl(KUTRACE_CMD_TEST, 0);
  if ((int64)retval < 0) {
    // KUtrace module/code is not available
    fprintf(stderr, "KUtrace module/code not available\n");
    return false;
  }
  return (retval == 1);
}

// Turn off tracing
// Complain and return false if module is not loaded
bool DoOff() {
  u64 retval = DoControl(KUTRACE_CMD_OFF, 0);
//fprintf(stderr, "DoOff DoControl = %016lx\n", retval);

  msleep(20);	/* Wait 20 msec for any pending tracing to finish */
  if (retval != 0) {
    // Module is not loaded
    fprintf(stderr, "KUtrace module/code not available\n");
    return false;
  }
  // Get stop time pair with tracing off
  if (stop_usec == 0) {GetTimePair(&stop_cycles, &stop_usec);}
//fprintf(stdout, "DoOff  GetTimePair %lx %lx\n", stop_cycles, stop_usec);
  return true;
}

// Turn on tracing
// Complain and return false if module is not loaded
bool DoOn() {
//fprintf(stderr, "DoOn\n");
  // Get start time pair with tracing off
  if (start_usec == 0) {GetTimePair(&start_cycles, &start_usec);}
//fprintf(stderr, "DoOn   GetTimePair %lx %lx\n", start_cycles, start_usec);
  u64 retval = DoControl(KUTRACE_CMD_ON, 0);
//fprintf(stderr, "DoOn DoControl = %016lx\n", retval);
  if (retval != 1) {
    // Module is not loaded
    fprintf(stderr, "KUtrace module/code not available\n");
    return false;
  }
  return true;
}



// We want to run all this stuff at kutrace_control startup and/or at reset, but just once
// per execution is sufficient once these values don't change until reboot.
// Current design is two-step:
//   one routine to capture all the info
//   second routine to insert into trace buffer
// We want the inserts to be fast and have no delays that might allow a process
// migration in the *middle* of building the initial name list. Doing so will
// confuse wraparound and my leave some events unnamed for the first few
// housand rawtoevent entries if some CPU blocks precede the remainder of
//  the name entries.

//   Linux kernel version
//   CPU model name
//   Hostname
//   Network link speed
//   Interrupt number to name mapping
//


// Initialize trace buffer with syscall/irq/trap names
// and processor model name, uname -rv
// Module must be loaded. Tracing must be off
void DoInit(const char* process_name) {
//fprintf(stderr, "DoInit\n");
  if (!TestModule()) {return;}		// No module loaded

  // AHHA. These can take more than 10msec to execute. so 20-bit time can wrap,
  // and we can get migrated to another CPU while we are blocked.
  // So we need to capture all the strings up front before creating the first trace
  // entry, and then insert all at once.
  GetKernelVersion(kernelversion, GetbufSize);
  GetModelName(modelname, GetbufSize);
  GetHostName(hostname, GetbufSize);
  GetLinkSpeed(linkspeed, GetbufSize);
  GetIrqNames(localirqpairs, irqnames);

  GetTimePair(&start_cycles, &start_usec);	// Now OK to look at time

  // Start trace buffer with a little trace environment information
  InsertVariableEntry(kernelversion, KUTRACE_KERNEL_VER, 0);
  InsertVariableEntry(modelname, KUTRACE_MODEL_NAME, 0);
  InsertVariableEntry(hostname, KUTRACE_HOST_NAME, 0);
  //InsertVariableEntry(linkspeed, KUTRACE_MBIT_SEC, 0);	(incomplete)

  // Add trap/irq/syscall names into front of trace
  EmitNames(PidNames, KUTRACE_PIDNAME);
  EmitNames(TrapNames, KUTRACE_TRAPNAME);
  EmitNames(localirqpairs, KUTRACE_INTERRUPTNAME);	// Running system interrupts 1st
  EmitNames(IrqNames, KUTRACE_INTERRUPTNAME);		// Default interrupt names   2nd
  EmitNames(Syscall64Names, KUTRACE_SYSCALL64NAME);
  EmitNames(ErrnoNames, KUTRACE_ERRNONAME);

  // Put current pid name into front of real part of trace
  int pid = getpid() & 0x0000ffff;
  InsertVariableEntry(process_name, KUTRACE_PIDNAME, pid);

  // And then establish that pid on this CPU
  //         T             N                       ARG
  u64 temp = (CLU(0) << 44) | ((u64)KUTRACE_USERPID << 32) | (pid);
  DoControl(~KUTRACE_CMD_INSERT1, temp);
}

// With tracing off, zero out the rest of each partly-used traceblock
// Module must be loaded. Tracing must be off
void DoFlush() {
//fprintf(stderr, "DoFlush\n");
  if (!TestModule()) {return;}		// No module loaded
  DoControl(KUTRACE_CMD_FLUSH, 0);
//fprintf(stderr, "DoFlush DoControl returned\n");
}

// Set up for a new tracing run
// Module must be loaded. Tracing must be off
void DoReset(u64 control_flags) {
  if (!TestModule()) {return;}		// No module loaded
  DoControl(KUTRACE_CMD_RESET, control_flags);

  start_usec = 0;
  stop_usec = 0;
  start_cycles = 0;
  stop_cycles = 0;
}

// Show some sort of tracing status
// Module must be loaded. Tracing may well be on
// If IPC,only 7/8 of the blocks are counted:
//  for every 64KB traceblock there is another 8KB IPCblock (and some wasted space)
void DoStat(u64 control_flags) {
  u64 retval = DoControl(KUTRACE_CMD_STAT, 0);
  double blocksize = kTraceBufSize * sizeof(u64);
  if ((control_flags & DO_IPC) != 0) {blocksize = (blocksize * 8) / 7;}
  fprintf(stderr, "Stat: %lld trace blocks used (%3.1fMB)\n",
          retval, (retval * blocksize) / (1024 * 1024));
}

#if 0
// OBSOLETE
// Called with the very first trace block, moduleversion >= 3
// This block has 12 words on the front, then a 3-word TimePairNum trace entry
void ExtractTimePair(u64* traceblock, int64* fallback_cycles, int64* fallback_usec) {
  u64 entry0 =       traceblock[12];
  u64 entry0_event = (entry0 >> 32) & 0xFFF;
  if ((entry0_event & 0xF0F) != KUTRACE_TIMEPAIR) {	// take out length nibble
    fprintf(stderr, "ExtractTimePair missing event\n");
    *fallback_cycles = 0;
    *fallback_usec =   0;
    return;
  }
  *fallback_cycles = traceblock[13];
  *fallback_usec =   traceblock[14];
}
#endif

// F(cycles) gives usec = base_usec + (cycles - base_cycles) * m;
typedef struct {
  u64 base_cycles;
  u64 base_usec;
  double m_slope;
} CyclesToUsecParams;

void SetParams(int64 start_cycles, int64 start_usec,
               int64 stop_cycles, int64 stop_usec, CyclesToUsecParams* param) {
  param->base_cycles = start_cycles;
  param->base_usec = start_usec;
  if (stop_cycles <= start_cycles) {stop_cycles = start_cycles + 1;}	// avoid zdiv
  param->m_slope = (stop_usec - start_usec) * 1.0 / (stop_cycles - start_cycles);
}

int64 CyclesToUsec(int64 cycles, const CyclesToUsecParams& param) {
  int64 delta_usec = (cycles - param.base_cycles) * param.m_slope;
  return param.base_usec + delta_usec;
}

#if 1
//VERYTEMP
static const int kMaxPrintBuffer = 256;
static char gTempPrintBuffer[kMaxPrintBuffer];

// Turn usec since the epoch into date_hh:mm:ss.usec
const char* FormatUsecDateTime(int64 us) {
  if (us == 0) {return "unknown";}  // Longer spelling: caller expecting date
  int32 seconds = us / 1000000;
  int32 usec = us - (seconds * 1000000);
  snprintf(gTempPrintBuffer, kMaxPrintBuffer, "%s.%06d",
           FormatSecondsDateTime(seconds), usec);
  return gTempPrintBuffer;
}

void DumpTimePair(const char* label, int64 cycles, int64 usec) {
  fprintf(stderr, "%s %016llx cy %016llx us => %s\n",
          label, cycles, usec, FormatUsecDateTime(usec));
}
#endif


// Dump the trace buffer to filename
// Module must be loaded. Tracing must be off
void DoDump(const char* fname) {
  bool livedump = DoTest();	// true if tracing is currently on

  // if (!TestModule()) {return;}		// No module loaded
  DoControl(KUTRACE_CMD_FLUSH, 0);

  // Start timepair is set by DoInit
  // Stop timepair is set by DoOff
  CyclesToUsecParams params;

  FILE* f = fopen(fname, "wb");
  if (f == NULL) {
    fprintf(stderr, "%s did not open\n", fname);
    return;
  }

  u64 traceblock[kTraceBufSize];
  u64 ipcblock[kIpcBufSize];
  // Get number of trace blocks as wordcount>>13
  // If tracing wraped around, the count is complemented
  bool did_wrap_around = false;
  u64 wordcount = DoControl(KUTRACE_CMD_GETCOUNT, 0);
  if ((s64)wordcount < 0) {
    wordcount = ~wordcount;
    did_wrap_around = true;
  }
  u64 blockcount = wordcount >> 13;	// 8K words per block
//fprintf(stderr, "wordcount = %ld\n", wordcount);
//fprintf(stderr, "blockcount = %ld\n", blockcount);

  // If module implements 4KB transfers, use those.
  bool use_4kb = (kIpcBufSize >= k4KBSize);
  use_4kb &= (DoControl(KUTRACE_CMD_VERSION, 0) >= kMin4KBModuleVersionNumber);

  // Live dump:
  // To trace kutrace_control itself dumping, live dump does:
  //   set the stop time pair, stop_cycles and stop_usec
  //   unconditionally dump the first 1.75MB of the trace buffer
  if (livedump) {
    GetTimePair(&stop_cycles, &stop_usec);
    blockcount = 28;
    fprintf(stderr, "Live dump of 1.75MB\n");
  }

  // Loop on trace blocks
  for (int i = 0; i < blockcount; ++i) {
    u64 k = i * kTraceBufSize;  // Trace Word number to fetch next
    u64 k2 = i * kIpcBufSize;  	// IPC Word number to fetch next

    // Extract 64KB trace block
    if (use_4kb) {
      for (int j = 0; j < kTraceBufSize; j += k4KBSize) {
        DoControl(KUTRACE_CMD_SET4KB, k);
        DoControl(KUTRACE_CMD_GET4KB, (u64)(&traceblock[j]));
        k += k4KBSize;
      }
    } else {
      for (int j = 0; j < kTraceBufSize; ++j) {
        traceblock[j] = DoControl(KUTRACE_CMD_GETWORD, k++);
      }
    }

    // traceblock[0] has cpu number and cycle counter
    // traceblock[1] has flags in top byte, then zeros
    // We put the reconstructed getimeofday value into traceblock[1]
    uint8 flags = traceblock[1] >> 56;
    bool this_block_has_ipc = ((flags & IPC_Flag) != 0);

    bool very_first_block = (i == 0);
    if (very_first_block) {
      // Fill in the tracefile version
      traceblock[1] |= ((kTracefileVersionNumber & VERSION_MASK) << 56);
      if (!did_wrap_around) {
        // The kernel exports the wrap flag in the first block before
        // it is known whether the trace actually wrapped.
        // It did not, so turn off that bit
        traceblock[1] &= ~(WRAP_Flag << 56);
      }

      uint64 block_0_cycle = traceblock[0] & CLU(0x00ffffffffffffff);

      // Get ready to reconstruct gettimeofday values for each traceblock
      SetParams(start_cycles, start_usec, stop_cycles, stop_usec, &params);

      // Fill in the start/stop timepairs we are using, so
      // downstream programs can also SetParams
      traceblock[2] = start_cycles;
      traceblock[3] = start_usec;
      traceblock[4] = stop_cycles;
      traceblock[5] = stop_usec;

      ////DumpTimePair("start", start_cycles, start_usec);
      ////DumpTimePair("stop ", stop_cycles, stop_usec);
    }	// End of very first block

    // Reconstruct the gettimeofday value for this block
    int64 block_cycles = traceblock[0] & CLU(0x00ffffffffffffff);
    int64 block_usec = CyclesToUsec(block_cycles, params);
    traceblock[1] |= (block_usec &  CLU(0x00ffffffffffffff));
    fwrite(traceblock, 1, sizeof(traceblock), f);

    ////fprintf(stderr, "[%d] ", i); DumpTimePair("block", block_cycles, block_usec);

    // For each 64KB traceblock that has IPC_Flag set, also read the IPC bytes
    if (this_block_has_ipc) {
      // Extract 8KB IPC block
      if (use_4kb) {
        for (int j = 0; j < kIpcBufSize; j += k4KBSize) {
          DoControl(KUTRACE_CMD_SET4KB, k2);
          DoControl(KUTRACE_CMD_GETIPC4KB, (u64)(&ipcblock[j]));
          k2 += k4KBSize;
        }
      } else {
        for (int j = 0; j < kIpcBufSize; ++j) {
          ipcblock[j] = DoControl(KUTRACE_CMD_GETIPCWORD, k2++);
        }
      }

      fwrite(ipcblock, 1, sizeof(ipcblock), f);
    }
  }
  fclose(f);

  fprintf(stdout, "  %s written (%3.1fMB)\n", fname, blockcount / 16.0);

  // Go ahead and set up for another trace
  DoControl(KUTRACE_CMD_RESET, 0);
}



// Exit this program
// Tracing must be off
void DoQuit() {
  DoOff();
  exit(0);
}

// Add a name of type n, value number, to the trace
void addname(uint64 eventnum, uint64 number, const char* name) {
  u64 temp[8];		// Buffer for name entry
  u64 bytelen = strlen(name);
  if (bytelen > 55) {bytelen = 55;}
  u64 wordlen = 1 + ((bytelen + 7) / 8);
  // Build the initial word
  u64 n_with_length = eventnum + (wordlen * 16);
  //             T             N                       ARG
  temp[0] = (CLU(0) << 44) | (n_with_length << 32) | (number);
  memset((char*)&temp[1], 0, 7 * sizeof(u64));
  memcpy((char*)&temp[1], name, bytelen);
  kutrace::DoControl(KUTRACE_CMD_INSERTN, (u64)&temp[0]);
}

// Create a Mark entry
void DoMark(u64 n, u64 arg) {
  //         T             N                       ARG
  u64 temp = (CLU(0) << 44) | (n << 32) | (arg &  CLU(0x00000000FFFFFFFF));
  DoControl(KUTRACE_CMD_INSERT1, temp);
}

// Create an arbitrary entry, returning 1 if tracing is on, <=0 otherwise
u64 DoEvent(u64 eventnum, u64 arg) {
  //         T             N                       ARG
  u64 temp = ((eventnum & CLU(0xFFF)) << 32) | (arg & CLU(0x00000000FFFFFFFF));
  return DoControl(KUTRACE_CMD_INSERT1, temp);
}

// Uppercase are mapped to lowercase
// All unexpected characters are mapped to '.'
//   - = 0x2D . = 0x2E / = 0x2F
// Base40 characters are _abcdefghijklmnopqrstuvwxyz0123456789-./
//                       0         1         2         3
//                       0123456789012345678901234567890123456789
// where the first is NUL.
static const char kToBase40[256] = {
   0,38,38,38, 38,38,38,38, 38,38,38,38, 38,38,38,38,
  38,38,38,38, 38,38,38,38, 38,38,38,38, 38,38,38,38,
  38,38,38,38, 38,38,38,38, 38,38,38,38, 38,37,38,39,
  27,28,29,30, 31,32,33,34, 35,36,38,38, 38,38,38,38,

  38, 1, 2, 3,  4, 5, 6, 7,  8, 9,10,11, 12,13,14,15,
  16,17,18,19, 20,21,22,23, 24,25,26,38, 38,38,38,38,
  38, 1, 2, 3,  4, 5, 6, 7,  8, 9,10,11, 12,13,14,15,
  16,17,18,19, 20,21,22,23, 24,25,26,38, 38,38,38,38,

  38,38,38,38, 38,38,38,38, 38,38,38,38, 38,38,38,38,
  38,38,38,38, 38,38,38,38, 38,38,38,38, 38,38,38,38,
  38,38,38,38, 38,38,38,38, 38,38,38,38, 38,38,38,38,
  38,38,38,38, 38,38,38,38, 38,38,38,38, 38,38,38,38,

  38,38,38,38, 38,38,38,38, 38,38,38,38, 38,38,38,38,
  38,38,38,38, 38,38,38,38, 38,38,38,38, 38,38,38,38,
  38,38,38,38, 38,38,38,38, 38,38,38,38, 38,38,38,38,
  38,38,38,38, 38,38,38,38, 38,38,38,38, 38,38,38,38,
};

static const char kFromBase40[40] = {
  '\0','a','b','c', 'd','e','f','g',  'h','i','j','k',  'l','m','n','o',
  'p','q','r','s',  't','u','v','w',  'x','y','z','0',  '1','2','3','4',
  '5','6','7','8',  '9','-','.','/',
};


// Unpack six characters from 32 bits.
// str must be 8 bytes. We somewhat-arbitrarily capitalize the first letter
char* Base40ToChar(u64 base40, char* str) {
  base40 &= CLU(0x00000000ffffffff);	// Just low 32 bits
  memset(str, 0, 8);
  bool first_letter = true;
  // First character went in last, comes out first
  int i = 0;
  while (base40 > 0) {
    u64 n40 = base40 % 40;
    str[i] = kFromBase40[n40];
    base40 /= 40;
    if (first_letter && (1 <= n40) && (n40 <= 26)) {
      str[i] &= ~0x20; 		// Uppercase it
      first_letter = false;
    }
    ++i;
  }
  return str;
}

// Pack six characters into 32 bits. Only use a-zA-Z0-9.-/
u64 CharToBase40(const char* str) {
  int len = strlen(str);
  // If longer than 6 characters, take only the first 6
  if (len > 6) {len = 6;}
  u64 base40 = 0;
  // First character goes in last, comes out first
  for (int i = len - 1; i >= 0; -- i) {
    base40 = (base40 * 40) + kToBase40[str[i]];
  }
  return base40;
}

}  // End anonymous namespace

bool kutrace::test() {return ::TestModule();}
void kutrace::go(const char* process_name) {::DoReset(0); ::DoInit(process_name); ::DoOn();}
void kutrace::goipc(const char* process_name) {::DoReset(1); ::DoInit(process_name); ::DoOn();}
void kutrace::stop(const char* fname) {::DoOff(); ::DoFlush(); ::DoDump(fname); ::DoQuit();}
void kutrace::mark_a(const char* label) {::DoMark(KUTRACE_MARKA, ::CharToBase40(label));}
void kutrace::mark_b(const char* label) {::DoMark(KUTRACE_MARKB, ::CharToBase40(label));}
void kutrace::mark_c(const char* label) {::DoMark(KUTRACE_MARKC, ::CharToBase40(label));}
void kutrace::mark_d(uint64 n) {::DoMark(KUTRACE_MARKD, n);}

// Returns number of words inserted 1..8, or
//   0 if tracing is off, negative if module is not not loaded
u64 kutrace::addevent(uint64 eventnum, uint64 arg) {return ::DoEvent(eventnum, arg);}

void kutrace::addname(uint64 eventnum, uint64 number, const char* name) {::addname(eventnum, number, name);}

void kutrace::msleep(int msec) {::msleep(msec);}
int64 kutrace::readtime() {return ::ku_get_cycles();}

// Go ahead and expose all the routines
const char* kutrace::Base40ToChar(u64 base40, char* str) {return ::Base40ToChar(base40, str);}
u64 kutrace::CharToBase40(const char* str) {return ::CharToBase40(str);}

u64 kutrace::DoControl(u64 command, u64 arg) {
  return ::DoControl(command, arg);
}
void kutrace::DoDump(const char* fname) {::DoDump(fname);}
u64  kutrace::DoEvent(u64 eventnum, u64 arg) {return ::DoEvent(eventnum, arg);}
void kutrace::DoFlush() {::DoFlush();}
void kutrace::DoInit(const char* process_name) {::DoInit(process_name);}
void kutrace::DoMark(u64 n, u64 arg) {::DoMark(n, arg);}
bool kutrace::DoTest() {return ::DoTest();}
bool kutrace::DoOff() {return ::DoOff();}
bool kutrace::DoOn() {return ::DoOn();}
void kutrace::DoQuit() {::DoQuit();}
void kutrace::DoReset(u64 doing_ipc){::DoReset(doing_ipc);}
void kutrace::DoStat(u64 control_flags) {::DoStat(control_flags);}
void kutrace::EmitNames(const NumNamePair* ipair, u64 n) {::EmitNames(ipair, n);}
u64 kutrace::GetUsec() {return ::GetUsec();}
const char* kutrace::MakeTraceFileName(const char* name, char* str) {
  return ::MakeTraceFileName(name, str);
}
bool kutrace::TestModule() {return ::TestModule();}





