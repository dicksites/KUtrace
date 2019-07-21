// Little user-mode library program to control kutracing 
// dick sites 2017.08.25
// 2017.11.16 dsites Updated to include instructions per cycle IPC flag
// 2018.05.01 dsites Updated to kutrace names
// 2018.05.08 dsites Updated to ARM64
// 2018.05.29 dsites Updated to KUTRACE_CMD_* spelling
// 2018.06.25 dsites Updated for wraparound
// 2019.02.19 dsites Updated time base
// 2019.03.03 dsites Updated to allow 64-bit syscalls 0..510 and 32-bit syscalls 512..1022
//

/*
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

#include <stdio.h>
#include <stdlib.h>     // exit
#include <string.h>
#include <time.h>	// nanosleep
#include <unistd.h>     // getpid gethostname syscall
#include <sys/time.h>   // gettimeofday
#include <sys/types.h>	

 
#ifndef __aarch64__
#include <x86intrin.h>
#endif

#include "basetypes.h"
#include "kutrace_control_names.h"	// PidNames, TrapNames, IrqNames, Syscall64Names
#include "kutrace_lib.h"

// All the real stuff is inside this anonymous namespace
namespace {

/* Outgoing arg to DoReset  */
#define DO_IPC 1
#define DO_WRAP 2

typedef long unsigned int u64;
typedef long signed int   s64;

/* For the flags byte in traceblock[1] */
#define IPC_Flag 0x80ul
#define WRAP_Flag 0x40ul
#define Unused2_Flag 0x20ul
#define Unused1_Flag 0x10ul
#define VERSION_MASK 0x0Ful


// Module/code must be at least this version number for us to run
static const u64 kMinModuleVersionNumber = 3;

// This defines the format of the resulting trace file
static const u64 kTracefileVersionNumber = 3;

// Number of u64 values per trace block
static const int kTraceBufSize = 8192;

// Number of u64 values per IPC block, one u8 per u64 in trace buf
static const int kIpcBufSize = kTraceBufSize >> 3;

// Globals for mapping cycles to gettimeofday
int64 start_cycles = 0;
int64 stop_cycles = 0;
int64 start_usec = 0;
int64 stop_usec = 0;

// Useful utility routines
int64 GetUsec() {
  struct timeval tv; gettimeofday(&tv, NULL);
  return (tv.tv_sec * 1000000l) + tv.tv_usec;
}


// Arm- or x86-specific timer
// Arm returns 32MHz counts: 31.25 ns each 
// x86-64 version returns rdtsc() >> 6 to give ~20ns resolution
inline int64 readtime(void) {
  u64 timer_value;
#if defined(__aarch64__)
  asm volatile("mrs %0, cntvct_el0" : "=r"(timer_value));
#elif defined(__x86_64__)
  timer_value = _rdtsc() >> 6;
#else
  BUILD_BUG_ON_MSG(1, "Define the time base for your architecture");
  timer_value = 0;
#endif
  return timer_value;
}


// Read time counter and gettimeofday() close together, returning both
void GetTimePair(int64* cycles, int64* usec) {
  int64 startcy, stopcy;
  int64 gtodusec, elapsedcy;
  // Do more than once if we get an interrupt or other big delay in the middle of the loop
  do {
    startcy = readtime();
    gtodusec = GetUsec();
    stopcy = readtime();
    elapsedcy = stopcy - startcy;
    // In a quick test on an Intel i3 chip, GetUsec() took about 150 cycles (50 nsec)
    //  Perhaps 2x this on Arm chips
    // printf("%ld elapsed cycles\n", elapsedcy);
  } while (elapsedcy > 256);  // About 8 usec at 32MHz
  *cycles = startcy;
  *usec = gtodusec;
}


// For the trace_control system call,
// arg is declared to be u64. In reality, it is either a u64 or
// a pointer to a u64, depending on the command. Caller casts as
// needed, and the command implementations in kutrace_mod
// cast back as needed.

// These numbers must exactly match the numbers in include/linux/kutrace.h
#define __NR_kutrace_control 1023
#define KUTRACE_SCHEDSYSCALL 511

u64 inline DoControl(u64 command, u64 arg)
{
  return syscall(__NR_kutrace_control, command, arg);
}




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
// str should holdatleast 256 bytes
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

  char hostnamestr[256];
  gethostname(hostnamestr, 256) ;
  hostnamestr[255] = '\0';

  int pid = getpid();

  sprintf(str, "%s_%s_%s_%d.trace", slash, timestr, hostnamestr, pid);
  return str; 
}           

// Add a list of names to the trace
// This depends on ~KUTRACE_CMD_INSERTN working even with tracing off. 
void EmitNames(const NumNamePair* ipair, u64 n) {
  u64 temp[9];		// One extra word for strcpy(56 bytes + '\n')
  const NumNamePair* pair = ipair;
  while (pair->name != NULL) {
    u64 bytelen = strlen(pair->name);
    if (bytelen > 56) {continue;}	// Error if too long. Drop
    u64 wordlen = 1 + ((bytelen + 7) / 8);
    // Build the initial word
    u64 n_with_length = n + (wordlen * 16);
    //         T             N                       ARG
    temp[0] = (0l << 44) | (n_with_length << 32) | (pair->number);
    memset(&temp[1], 0, 8 * sizeof(u64));
    strcpy((char*)&temp[1], pair->name);
    DoControl(~KUTRACE_CMD_INSERTN, (u64)&temp[0]);
    ++pair;
  }
}

// This depends on ~TRACE_INSERTN working even with tracing off. 
void InsertTimePair(int64 cycles, int64 usec) {
  u64 temp[8];		// Always 8 words for TRACE_INSERTN
  u64 n_with_length = KUTRACE_TIMEPAIR + (3 << 4);
  temp[0] = (0ll << 44) | (n_with_length << 32);
  temp[1] = cycles;
  temp[2] = usec;
  DoControl(~KUTRACE_CMD_INSERTN, (u64)&temp[0]);
}



// Return false if the module is not loaded or too old. No delay. No side effect on time.
bool TestModule() {
  // If module is not loaded, syscall 511 returns -ENOSYS (= -38)
  ////u64 retval = DoControl(KUTRACE_CMD_VERSION, 0);
  u64 retval = DoControl(KUTRACE_CMD_VERSION, 3333);
//VERYTEMP
fprintf(stderr, "TestModule DoControl = %016lx\n", retval);
  if ((int64)retval < 0) {
    // Module is not loaded
    fprintf(stderr, "KUtrace module/code not loaded\n");
    return false;
  }
  if (retval < kMinModuleVersionNumber) {
    // Module is loaded but older version
    fprintf(stderr, "KUtrace module/code is version %ld. Need at least %ld\n",
      retval, kMinModuleVersionNumber);
    return false;
  }
  fprintf(stderr, "TestModule KUtrace module/code is version %ld.\n", retval);
  return true;
}


// Return true if module is loaded and tracing is on, else false
// CMD_TEST returns -ENOSYS (= -38) if not a tracing kernel
// else returns 0 if tracing is off
// else returns 1 if tracing is on
bool DoTest() {
  u64 retval = DoControl(KUTRACE_CMD_TEST, 0);
//VERYTEMP
fprintf(stderr, "DoTest DoControl = %016lx\n", retval);
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
fprintf(stderr, "DoOff DoControl = %016lx\n", retval);

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
fprintf(stderr, "DoOn\n");
  // Get start time pair with tracing off
  if (start_usec == 0) {GetTimePair(&start_cycles, &start_usec);}
//fprintf(stdout, "DoOn   GetTimePair %lx %lx\n", start_cycles, start_usec);
  u64 retval = DoControl(KUTRACE_CMD_ON, 0);
//fprintf(stderr, "DoOn DoControl = %016lx\n", retval);
  if (retval != 1) {
    // Module is not loaded
    fprintf(stderr, "KUtrace module/code not available\n");
    return false;
  }
  return true;
}


// Initialize trace buffer with syscall/irq/trap names
// Module must be loaded. Tracing must be off
void DoInit(const char* process_name) {
fprintf(stderr, "DoInit\n");
  if (!TestModule()) {return;}		// No module loaded
  GetTimePair(&start_cycles, &start_usec);
//fprintf(stdout, "DoInit GetTimePair %lx %lx\n", start_cycles, start_usec);
  // Insert the timepair as a trace entry. 
  // This is a hedge against separate programs starting (wraparound) tracing 
  // and stopping tracing. If this happens, start_usec will be zero at DoOff().
  
  // We want this to be the very first trace entry so we can find it easily
  InsertTimePair(start_cycles, start_usec); 

  // Put trap/irq/syscall names into front of trace
  EmitNames(PidNames, KUTRACE_PIDNAME);
  EmitNames(TrapNames, KUTRACE_TRAPNAME);
  EmitNames(IrqNames, KUTRACE_INTERRUPTNAME);
  EmitNames(Syscall64Names, KUTRACE_SYSCALL64NAME);

  // This depends on ~KUTRACE_CMD_INSERTN working even with tracing off. 
  // Put current pid name into front of trace
  int pid = getpid() & 0x0000ffff;
  u64 temp[3];
  u64 n_with_length = KUTRACE_PIDNAME + (3 << 4);
  n_with_length = KUTRACE_PIDNAME + (3 << 4);
  //         T             N                       ARG
  temp[0] = (0ll << 44) | (n_with_length << 32) | (pid);
  temp[1] = 0;
  temp[2] = 0;
  if (strlen(process_name) < 16) {
    strcpy((char*)&temp[1], process_name);
  } else {
    memcpy((char*)&temp[1], process_name, 16);
  }
  DoControl(~KUTRACE_CMD_INSERTN, (u64)&temp[0]);

  // This depends on ~KUTRACE_CMD_INSERT1 working even with tracing off. 
  // And then establish that pid on this CPU
  n_with_length = KUTRACE_USERPID;
  //         T             N                       ARG
  temp[0] = (0l << 44) | (n_with_length << 32) | (pid);
  DoControl(~KUTRACE_CMD_INSERT1, temp[0]);
}

// With tracing off, zero out the rest of each partly-used traceblock
// Module must be loaded. Tracing must be off
void DoFlush() {
fprintf(stderr, "DoFlush\n");
  if (!TestModule()) {return;}		// No module loaded
  DoControl(KUTRACE_CMD_FLUSH, 0);
fprintf(stderr, "DoFlush DoControl returned\n");

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
  fprintf(stderr, "Stat: %ld trace blocks used (%3.1fMB)\n", 
          retval, (retval * blocksize) / (1024 * 1024));
}

// Called with the very first trace block, moduleversion >= 3
// This block has 12 words on the front, then a 3-word TimePairNum trace entry
void ExtractTimePair(u64* traceblock, int64* fallback_cycles, int64* fallback_usec) {
  *fallback_cycles = traceblock[13];
  *fallback_usec =   traceblock[14];
}

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



// Dump the trace buffer to filename
// Module must be loaded. Tracing must be off
void DoDump(const char* fname) {
  // if (!TestModule()) {return;}		// No module loaded
  DoControl(KUTRACE_CMD_FLUSH, 0);

  // Start timepair is set by DoInit
  // Stop timepair is set by DoOff
  // If start_cycles is zero, we got here directly without calling DoInit, 
  // which was done in some earlier run of this program. In that case, go 
  // find the start pair as the first real trace entry in the first trace block.
  CyclesToUsecParams params;

  FILE* f = fopen(fname, "wb");
  if (f == NULL) {
    fprintf(stderr, "%s did not open\n", fname);
    return;
  }

  u64 traceblock[kTraceBufSize];
  u64 ipcblock[kIpcBufSize];
  // Get number of trace blocks
  bool did_wrap_around = false;
  u64 wordcount = DoControl(KUTRACE_CMD_GETCOUNT, 0);
  if ((s64)wordcount < 0) {
    wordcount = ~wordcount;
    did_wrap_around = true;
  }
  u64 blockcount = wordcount >> 13;
fprintf(stderr, "wordcount = %ld\n", wordcount);
fprintf(stderr, "blockcount = %ld\n", blockcount);

  // Loop on trace blocks
  for (int i = 0; i < blockcount; ++i) {
    u64 k = i * kTraceBufSize;  // Trace Word number to fetch next
    u64 k2 = i * kIpcBufSize;  	// IPC Word number to fetch next
    
    // Extract 64KB trace block
    for (int j = 0; j < kTraceBufSize; ++j) {
      traceblock[j] = DoControl(KUTRACE_CMD_GETWORD, k++);
    }

    // traceblock[0] has cpu number and cycle counter
    // traceblock[1] has flags in top byte, then zeros
    // We put the reconstructed getimeofday value into traceblock[1] 
    uint8 flags = traceblock[1] >> 56;
    bool this_block_has_ipc = ((flags & IPC_Flag) != 0);

    // For very first block, insert value of m as a double, for dump program to use
    // and clear traceblock[3], reserved for future use
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
      // Extract the fallback start timepair
      int64 fallback_usec, fallback_cycles;
      ExtractTimePair(traceblock, &fallback_cycles, &fallback_usec);
      if (start_usec == 0) {
        start_usec = fallback_usec;
        start_cycles = fallback_cycles;
      }

      uint64 block_0_cycle = traceblock[0] & 0x00fffffffffffffful;

      // Get ready to reconstruct gettimeofday values for each traceblock
      SetParams(start_cycles, start_usec, stop_cycles, stop_usec, &params);

      // Fill in the start/stop timepairs we are using, so
      // downstream programs can also SetParams
      traceblock[2] = start_cycles;
      traceblock[3] = start_usec;
      traceblock[4] = stop_cycles;
      traceblock[5] = stop_usec;
    }

    // Reconstruct the gettimeofday value for this block
    int64 block_cycles = traceblock[0] & 0x00ffffffffffffffl;
    int64 block_usec = CyclesToUsec(block_cycles, params);
    traceblock[1] |= (block_usec & 0x00ffffffffffffffl);
    fwrite(traceblock, 1, sizeof(traceblock), f);

    // For each 64KB traceblock that has IPC_Flag set, also read the IPC bytes
    if (this_block_has_ipc) {
      // Extract 8KB IPC block
      for (int j = 0; j < kIpcBufSize; ++j) {
        ipcblock[j] = DoControl(KUTRACE_CMD_GETIPCWORD, k2++);
      }
      fwrite(ipcblock, 1, sizeof(ipcblock), f);
    }
  }
  fclose(f);

  fprintf(stdout, "  %s written (%3.1fMB)\n", fname, blockcount / 16.0);

  // Go ahead and set up for another trace
  DoControl(KUTRACE_CMD_RESET, 0);
}



#if 0
//************************ old version

// Dump the trace buffer to filename
// Module must be loaded. Tracing must be off
void DoDump(const char* fname) {
  if (!TestModule()) {return;}		// No module loaded
  DoControl(KUTRACE_CMD_FLUSH, 0);
fprintf(stderr, "DoDump flush DoControl returned\n");

  // Calculate mapping from cycles to usec. Anding is because cycles are
  // stored with cpu# in the high byte of traceblock[0]
  int64 start_cycles56 = start_cycles & 0x00fffffffffffffl;
  int64 stop_cycles56 =  stop_cycles & 0x00ffffffffffffffl;
  double m = (stop_usec - start_usec) * 1.0;
  if ((stop_cycles56 - start_cycles56) > 0.0) {m = m / (stop_cycles56 - start_cycles56);}
  // We expect m to be on the order of 1/3000

  // June 2018 Newer design -- put TimePairs in trace at front and get pair now.
  // Put both pairs in first traceblock [2,3] and [4,5] before writing to disk
  // Depends on finding start pair early in trace in first block
  int64 dump_cycles, dump_usec;
  GetTimePair(&dump_cycles, &dump_usec);

  FILE* f = fopen(fname, "wb");
  if (f == NULL) {
    fprintf(stderr, "%s did not open\n", fname);
    return;
  }

  u64 traceblock[kTraceBufSize];
  u64 ipcblock[kIpcBufSize];
  // get number of trace blocks
  u64 wordcount = DoControl(KUTRACE_CMD_GETCOUNT, 0);
fprintf(stderr, "DoDump wordcount DoControl = %016lx\n", wordcount);
  u64 blockcount = wordcount >> 13;

  // Loop on trace blocks
  for (int i = 0; i < blockcount; ++i) {
    u64 k = i * kTraceBufSize;  // Trace Word number to fetch next
    u64 k2 = i * kIpcBufSize;  	// IPC Word number to fetch next
    
    // Extract 64KB trace block
    for (int j = 0; j < kTraceBufSize; ++j) {
      traceblock[j] = DoControl(KUTRACE_CMD_GETWORD, k++);
    }

    // traceblock[0] already has cycle counter and cpu#

    // traceblock[1] already has flags in top byte
    uint8 flags = traceblock[1] >> 56;
    bool this_block_has_ipc = ((flags & IPC_Flag) != 0);

    // Set the interpolated gettimeofday time 
    int64 mid_usec = (traceblock[0] & 0x00ffffffffffffffl) - start_cycles56;
    mid_usec *= m;
    traceblock[1] |= mid_usec + start_usec;

    // For very first block, insert value of m as a double, for dump program to use
    // and clear traceblock[3], reserved for future use
    if (i == 0) {
      // We expect the first entry, at [8] to be 3 words of timepair
// +-------------------+-----------+-------------------------------+
// | timestamp         | event     |              arg              |
// +-------------------+-----------+-------------------------------+
// |   cycle counter value                                         |
// +---------------------------------------------------------------+
// |   matching gettimeofday value                                 |
// +---------------------------------------------------------------+
      traceblock[2] = traceblock[9];	// init_cycles
      traceblock[3] = traceblock[10];	// init_usec
      traceblock[4] = dump_cycles;
      traceblock[5] = dump_usec;
    }
    fwrite(traceblock, 1, sizeof(traceblock), f);

    // For each 64KB traceblock that has IPC_Flag set, also read the IPC bytes
    if (this_block_has_ipc) {
      // Extract 8KB IPC block
      for (int j = 0; j < kIpcBufSize; ++j) {
        ipcblock[j] = DoControl(KUTRACE_CMD_GETIPCWORD, k2++);
      }
      fwrite(ipcblock, 1, sizeof(ipcblock), f);
    }
  }
  fclose(f);
  fprintf(stderr, "%s written, %ldKB\n", fname, blockcount * 64);


  // Go ahead and set up for another trace
  DoControl(KUTRACE_CMD_RESET, 0);
fprintf(stderr, "DoDump reset DoControl returned\n");

}
//************************ old version
#endif



// Exit this program
// Tracing must be off
void DoQuit() {
  DoOff();
  exit(0);
}

// Create a Mark entry
void DoMark(u64 n, u64 arg) {
  //         T             N                       ARG
  u64 temp = (0l << 44) | (n << 32) | (arg & 0x00000000FFFFFFFFl);
  DoControl(KUTRACE_CMD_INSERT1, temp);
}

// Create a Mark entry
void DoEvent(u64 eventnum, u64 arg) {
  //         T             N                       ARG
  u64 temp = ((eventnum & 0xFFF) << 32) | (arg & 0x00000000FFFFFFFFl);
  DoControl(KUTRACE_CMD_INSERT1, temp);
}

// Uppercase mapped to lowercase
// All unexpected characters mapped to '-'
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

//VERYTEMP
//Make executable bigger
static const u64 foobar[1024] = {22, 33, 44, 55, 66};


// Unpack six characters from 32 bits.
// str must be 8 bytes. We somewhat-arbitrarily capitalize the first letter
char* Base40ToChar(u64 base40, char* str) {
  base40 &= 0x00000000fffffffflu;	// Just low 32 bits
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

void kutrace::addevent(uint64 eventnum, uint64 arg) {::DoEvent(eventnum, arg);}
void kutrace::msleep(int msec) {::msleep(msec);}
int64 kutrace::readtime() {return ::readtime();}

// Go ahead and expose all the routines
const char* kutrace::Base40ToChar(u64 base40, char* str) {return ::Base40ToChar(base40, str);}
u64 kutrace::CharToBase40(const char* str) {return ::CharToBase40(str);}

void kutrace::DoControl(u64 command, u64 arg) {
  //VERYTEMP
  fprintf(stderr, "kutrace::DoControl(%ld, %ld)\n", command, arg);

  u64 retval = ::DoControl(command, arg);

  //VERYTEMP
  fprintf(stderr, "  returned %016lx\n", retval);
}
void kutrace::DoDump(const char* fname) {::DoDump(fname);}
void kutrace::DoEvent(u64 eventnum, u64 arg) {::DoEvent(eventnum, arg);}
void kutrace::DoFlush() {::DoFlush();}
void kutrace::DoInit(const char* process_name) {::DoInit(process_name);}
void kutrace::DoMark(u64 n, u64 arg) {::DoMark(n, arg);}
bool kutrace::DoTest() {::DoTest();}
bool kutrace::DoOff() {::DoOff();}
bool kutrace::DoOn() {::DoOn();}
void kutrace::DoQuit() {::DoQuit();}
void kutrace::DoReset(u64 doing_ipc){::DoReset(doing_ipc);}
void kutrace::DoStat(u64 control_flags) {::DoStat(control_flags);}
void kutrace::EmitNames(const NumNamePair* ipair, u64 n) {::EmitNames(ipair, n);}
void kutrace::GetUsec() {::GetUsec();}
const char* kutrace::MakeTraceFileName(const char* name, char* str) {
  return ::MakeTraceFileName(name, str);
}
bool kutrace::TestModule() {return ::TestModule();}


 


