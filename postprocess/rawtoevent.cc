// Little program to turn raw binary dclab trace files into Ascii event listings
// The main work is turning truncated cycle times into multiples of 10ns
// dick sites 2016.10.18
// dick sites 2017.08.10
//  move position of event to just after duration; export names
// dick sites 2017.11.18
//  add optional instructions per cycle IPC processing
//
// dsites 2018.05.30 update to kutrace names
// dsites 2018.06.05 update to version 3 with ARM timing, timepairs
// dsites 2019.03.04 update to Linux 4.19 names
//
// Input has filename like 
//   kutrace_control_20170821_095154_dclab-1_2056.trace
//
// Compile with
//  g++ -O2 rawtoevent.cc from_base40.cc kutrace_lib.cc -o rawtoevent
//
//  od -Ax -tx8z -w32 foo.trace
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


#include <map>
#include <set>
#include <string>

#include <stdio.h>
#include <stdlib.h>     // exit
#include <string.h>
#include <time.h>
#include <unistd.h>     // getpid gethostname
#include <sys/time.h>   // gettimeofday
#include <sys/types.h>

#include "../control/basetypes.h"
#include "from_base40.h"
#include "../control/kutrace_control_names.h"
#include "../control/kutrace_lib.h"


/* Amount to shift cycle counter to get 20-bit timestamps */
/* 4 bits = ~ 2.56 GHz/16 ~ 6nsec tick resolution */
/* 6 bits = ~ 2.56 GHz/64 ~ 24nsec tick resolution */
/* 8 bits = ~ 2.56 GHz/256 ~ 100nsec tick resolution */
/* 12 bits = ~ 2.56 GHz/4096 ~ 1.6 usec tick resolution */
/* THIS MUST MATCH the value in the kernel tracing module/code */

// Global for debugging
bool verbose = false;


//VERYTEMP
//static const uint64 FINDME = 1305990942;
static const uint64 FINDME = 0;

static const bool TRACEWRAP = false;


// Version 3 all values are pre-shifted

#define IPC_Flag     0x80
#define WRAP_Flag    0x40
#define Unused2_Flag 0x20
#define Unused1_Flag 0x10
#define VERSION_MASK 0x0F

#define RDTSC_SHIFT 0 
#define OLD_RDTSC_SHIFT 6


// Module, control must be at least version 3 
static const int kRawVersionNumber = 3;

static const char* kIdleName = "-idle-";


// Very first block layout June 2018, called 12/6 headers
// Enables wraparound
// flags = x3 hex
//   +-------+-----------------------+-------------------------------+
//   | cpu#  |                  cycle counter                        | 0 module
//   +-------+-----------------------+-------------------------------+
//   | flags |                  gettimeofday                         | 1 DoDump
//   +-------+-----------------------+-------------------------------+
//   |                      start cycle counter                      | 2 DoDump
//   +-------------------------------+-------------------------------+
//   |                      start gettimeofday                       | 3 DoDump
//   +-------------------------------+-------------------------------+
//   |                       stop cycle counter                      | 4 DoDump
//   +-------------------------------+-------------------------------+
//   |                       stop gettimeofday                       | 5 DoDump
//   +-------------------------------+-------------------------------+
//   |                          u n u s e d                          | 6
//   +-------------------------------+-------------------------------+
//   |                          u n u s e d                          | 7
//   +-------------------------------+-------------------------------+
//   +-------------------------------+-------------------------------+
//   |           u n u s e d         |            PID                | 8  module
//   +-------------------------------+-------------------------------+
//   |                          u n u s e d                          | 9  module
//   +-------------------------------+-------------------------------+
//   |                                                               | 10 module
//   +                            pidname                            +
//   |                                                               | 11 module
//   +-------------------------------+-------------------------------+
//   |    followed by trace entries...                               |
//   ~                                                               ~
//
//
// All other blocks layout June 2018
//   +-------+-----------------------+-------------------------------+
//   | cpu#  |                  cycle counter                        | 0 module
//   +-------+-----------------------+-------------------------------+
//   | flags |                  gettimeofday                         | 1 DoDump
//   +-------+-----------------------+-------------------------------+
//   |           u n u s e d         |            PID                | 2 module
//   +-------------------------------+-------------------------------+
//   |                          u n u s e d                          | 3 module
//   +-------------------------------+-------------------------------+
//   |                                                               | 4 module
//   +                            pidname                            +
//   |                                                               | 5 module
//   +-------------------------------+-------------------------------+
//   |    followed by trace entries...                               |
//   ~                                                               ~
//




// MWAIT notes:
// $ cat /proc/cpuinfo
//   processor	: 0
//   vendor_id	: GenuineIntel
//   cpu family	: 6
//   model		: 60  ==> 0x3C
//   model name	: Intel(R) Celeron(R) CPU G1840 @ 2.80GHz
//
// ./drivers/idle/intel_idle.c
//   ICPU(0x3c, idle_cpu_hsw),

// static struct cpuidle_state hsw_cstates[] = {
// These latencies are documented as usec, but I think they are 100ns increments...
//  mwait(32), hda_29 13.9us  table: 133
//  mwait(32), hda_29 13.3us  table: 133
//  mwait(16), hda_29  4.0us  table: 33
//  mwait(16), hda_29  3.75us table: 33
//  mwait(1),  hda_29  1.74us table: 10
//  mwait(1),  hda_29  1.76us table: 10

//   "C1-HSW",  0x00, .exit_latency = 2,        // usec ?
//   "C1E-HSW", 0x01, .exit_latency = 10,
//   "C3-HSW",  0x10, .exit_latency = 33,
//   "C6-HSW",  0x20, .exit_latency = 133,
//   "C7s-HSW", 0x32, .exit_latency = 166,
//   "C8-HSW",  0x40, .exit_latency = 300,
//   "C9-HSW",  0x50, .exit_latency = 600,
//   "C10-HSW", 0x60, .exit_latency = 2600,



using std::map;
using std::set;
using std::string;

static double kDefaultSlope = 0.000285714;  // 1/3500, dclab-3 at 3.5 GHz

// Number of uint64 values per trace block
static const int kTraceBufSize = 8192;
// Number trace blocks per MB
static const double kTraceBlocksPerMB = 16.0;

static const int kMaxCPUs = 80;

static const char* soft_irq_name[] = {
  "hi", "timer", "tx", "rx",   "block", "irq_p", "taskl", "sched", 
  "hrtim", "rcu", "", "",    "", "", "", ""
};


typedef map<uint64, string> U64toString;


// These all use a single static buffer. In real production code, these would 
// all be std::string values, or something else at least as safe.
static const int kMaxDateTimeBuffer = 32;
static char gTempDateTimeBuffer[kMaxDateTimeBuffer];

static const int kMaxPrintBuffer = 256;
static char gTempPrintBuffer[kMaxPrintBuffer];

// F(cycles) gives usec = base_usec + (cycles - base_cycles) * m;
typedef struct {
  uint64 base_cycles;
  uint64 base_usec;
  uint64 base_cycles10;
  uint64 base_nsec10;
  double m_slope;
  double m_slope_nsec10;
} CyclesToUsecParams;

void SetParams(int64 start_cycles, int64 start_usec, 
               int64 stop_cycles, int64 stop_usec, CyclesToUsecParams* params) {
  params->base_cycles = start_cycles;
  params->base_usec = start_usec;
  if (stop_cycles <= start_cycles) {stop_cycles = start_cycles + 1;}	// avoid zdiv
  params->m_slope = (stop_usec - start_usec) * 1.0 / (stop_cycles - start_cycles);
  params->m_slope_nsec10 = params->m_slope * 100.0;
  if (verbose) {
    fprintf(stdout, "SetParams maps %18ldcy ==> %18ldus\n", start_cycles, start_usec);
    fprintf(stdout, "SetParams maps %18ldcy ==> %18ldus\n", stop_cycles, stop_usec);
    fprintf(stdout, "          diff %18ldcy ==> %18ldus\n", stop_cycles - start_cycles, stop_usec - start_usec);
    // Assume that cy increments every 64 CPU cycles
    fprintf(stdout, "SetParams slope %f us/cy (%f MHz)\n", params->m_slope, 64.0/params->m_slope);
  }
}

void SetParams10(int64 start_cycles10, int64 start_nsec10, CyclesToUsecParams* params) {
  params->base_cycles10 = start_cycles10;
  params->base_nsec10 = start_nsec10;
  if (verbose) {
    fprintf(stdout, "SetParams10 maps %16ldcy ==> %ldns10\n", start_cycles10, start_nsec10);
  }
}

int64 CyclesToUsec(int64 cycles, const CyclesToUsecParams& params) {
  int64 delta_usec = (cycles - params.base_cycles) * params.m_slope;
  return params.base_usec + delta_usec;
}

uint64 CyclesToNsec10(uint64 cycles, CyclesToUsecParams& params) {
  int64 delta_nsec10 = (cycles - params.base_cycles10) * params.m_slope_nsec10;
  return params.base_nsec10 + delta_nsec10;
}

int64 UsecToCycles(int64 usec, CyclesToUsecParams& params) {
  int64 delta_cycles = (usec - params.base_usec);
  delta_cycles /= params.m_slope;  // Combining above fails to convert double=>int64
  return params.base_cycles + delta_cycles;
}


// Turn seconds since the epoch into date_hh:mm:ss
// Not valid after January 19, 2038
const char* FormatSecondsDateTime(int32 sec) {
  if (sec == 0) {return "unknown";}  // Longer spelling: caller expecting date
  time_t tt = sec;
  struct tm* t = localtime(&tt);
  sprintf(gTempDateTimeBuffer, "%04d-%02d-%02d_%02d:%02d:%02d", 
         t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, 
         t->tm_hour, t->tm_min, t->tm_sec);
  return gTempDateTimeBuffer;
}

// Turn usec since the epoch into date_hh:mm:ss.usec
const char* FormatUsecDateTime(int64 us) {
  if (us == 0) {return "unknown";}  // Longer spelling: caller expecting date
  int32 seconds = us / 1000000;
  int32 usec = us - (seconds * 1000000);
  snprintf(gTempPrintBuffer, kMaxPrintBuffer, "%s.%06d", 
           FormatSecondsDateTime(seconds), usec);
  return gTempPrintBuffer;
}

// We wrapped if high bit of a is 1 and high bit of b is 0
inline bool Wrapped(uint64 prior, uint64 now) {
  return (((prior & ~now) & 0x80000) != 0);  
}
 
// A user-mode-execution event is the pid number plus 64K
uint64 PidToEvent(uint64 pid) {return pid + 0x10000;}
uint64 EventToPid(uint64 event) {return event - 0x10000;}

// Return true if the event is user-mode execution 
inline bool is_contextswitch(uint64 event) {return (event == KUTRACE_USERPID);}

// Return true if the event is the idle task, pid 0 
inline bool is_idle(uint64 event) {return (event == 0x10000);}

// Return true if the event is user-mode execution 
inline bool is_usermode(uint64 event) {return (event > 0xffff) && !is_idle(event);}

// Return true if the event is a syscall/interrupt/trap 
inline bool is_call(uint64 event) {return (event <= 0xffff) && (KUTRACE_TRAP <= event) && ((event & 0x0200) == 0);}

// Return true if the event is an optimized syscall/interrupt/trap with included return 
inline bool is_opt_call(uint64 event, uint64 delta_t) {return (delta_t > 0) && is_call(event);}

// Return true if the event is a syscall/interrupt/trap return
inline bool is_return(uint64 event) {return (event <= 0xffff) && (KUTRACE_TRAP <= event) && ((event & 0x0200) != 0);}

// Return true if the event is a name definition
inline bool is_timepair(uint64 event) {return (event & ~0x0f0) == KUTRACE_TIMEPAIR;}

// Return true if the event is a name definition
inline bool is_namedef(uint64 event) {return (0x0010 <= event) && (event <= 0x01ff);}

// Return true if the name event is a PID name definition
inline bool is_pidnamedef(uint64 event) {return (event & 0x00f) == 0x002;}

// Return true if the event is a special marker (but not UserPidNum)
inline bool is_special(uint64 event) {return (0x0200 < event) && (event < 0x0210);}

// Return true if the event is mark_a .. mark_d
inline bool is_mark(uint64 event) {return ((0x020A <= event) && (event <= 0x020D));}

// Return true if the event is mark_a mark_b mark_c
inline bool is_mark_abc(uint64 event) {
  return (event == 0x020A) || (event == 0x020B) || (event == 0x020C);
}

// Return true if the event is rpcreq, rpcresp, rpcmid, rpcrxpkt, rpxtxpkt,
inline bool has_rpcid(uint64 event) {
  return (KUTRACE_RPCIDREQ <= event) && (event <= KUTRACE_RPCIDTXPKT);
}

// Return true if this event is irq call/ret to bottom half soft_irq handler (BH)
inline bool is_bottom_half(uint64 event) {return (event & ~0x0200) == 0x5FF;}



int TracefileVersion(uint8 flags) {
  return flags & VERSION_MASK;
}

int HasIPC(uint8 flags) {
  return (flags & IPC_Flag) != 0;
}

int HasWraparound(uint8 flags) {
  return (flags & WRAP_Flag) != 0;
}


// time dur event pid name(event)
void OutputName(FILE* f, uint64 nsec10, uint64 nameinsert, const char* pidname) {
  uint64 len = ((strlen(pidname) + 7) >> 3) + 1;
  uint64 duration = 1;
  uint64 event = KUTRACE_PIDNAME + (len << 4);
  fprintf(f, "%ld %ld %ld %ld %s\n", 
          nsec10, duration, event, nameinsert, pidname);
  // Also put the name at the very front of the sorted event list
  fprintf(f, "%ld %ld %ld %ld %s\n", 
          -1l, duration, event, nameinsert, pidname);
}

// time dur event cpu  pid rpc  arg retval IPC name(event)
void OutputEvent(FILE* f, 
                 uint64 nsec10, uint64 duration, uint64 event, uint64 current_cpu,
                 uint64 pid, uint64 rpc, 
                 uint64 arg, uint64 retval, int ipc, const char* name) {
  fprintf(f, "%ld %ld %ld %ld  %ld %ld  %ld %ld %d %s (%lx)\n", 
          nsec10, duration, event, current_cpu, 
          pid, rpc, 
          arg, retval, ipc, name, event);
}

// Add the pid# to the end of user-mode name, if not already there
string AppendPid(const string& name, uint64 pid) {
  char pidnum_temp[16];
  sprintf(pidnum_temp, ".%ld", pid & 0xffff);
  if (strstr(name.c_str(), pidnum_temp) == NULL) {
    return name + string(pidnum_temp);
  }
  return name;
}
 

//
// Usage: rawtoevent <trace file name>
//
int main (int argc, const char** argv) {
  // Some statistics
  uint64 base_usec_timestamp;
  uint64 event_count = 0;
  uint64 lo_timestamp = 0x7FFFFFFFFFFFFFFFl;
  uint64 hi_timestamp = 0;
  set<uint64> unique_cpus;
  set<uint64> unique_pids;
  uint64 ctx_switches = 0;
  uint64 total_marks = 0;
  uint64 events_by_type[16];
  memset(events_by_type, 0, 16 * sizeof(uint64));

  uint64 current_cpu = 0;
  uint64 traceblock[kTraceBufSize];	// 8 bytes per trace entry
  uint8 ipcblock[kTraceBufSize];	// One byte per trace entry

  uint64 current_pid[kMaxCPUs];	// Keep track of current PID on each of 16 cores
  uint64 current_rpc[kMaxCPUs]; // Keep track of current rpcid on each of 16 cores
  U64toString names;

  // Start timepair is set by DoInit
  // Stop timepair is set by DoOff
  // If start_cycles is zero, we got here directly without calling DoInit, 
  // which was done in some earlier run of this program. In that case, go 
  // find the start pair as the first real trace entry in the first trace block.
  CyclesToUsecParams params;

  // Events are 0..64K-1 for everything except context switch.
  // Context switch events are 0x10000 + pid
  // Initialize idle process name, pid 0
  names[0x10000] = string(kIdleName);
  
  for (int i = 0; i < 16; ++i) {
    current_pid[i] = 0; 
    current_rpc[i] = 0;
  }

  // For converting cycle counts to multiples of 100ns
  double m = kDefaultSlope;

  FILE* f = stdin;
  if (argc >= 2) {
    f = fopen(argv[1], "rb");
    if (f == NULL) {
      fprintf(stderr, "%s did not open\n", argv[1]);
      exit(0);
    }
  }

  // Pick up verbose flag
  for (int i = 1; i < argc; ++i) {if (strcmp(argv[i], "-v") == 0) {verbose = true;}}

  int blocknumber = 0;
  uint64 base_minute_usec, base_minute_cycle, base_minute_shift;
  bool unshifted_word_0 = false;

  // Need this to sort in front of allthe timestamps
  fprintf(stdout, "# ## VERSION: %d\n", kRawVersionNumber);
  uint8 all_flags = 0;	// They should all be the same
  uint8 first_flags;	// Just first block has tracefile version number


  while (fread(traceblock, 1, sizeof(traceblock), f) != 0) {
    // Need first [1] line to get basetime in later steps
    // TODO: Move this to a stylized BASETIME comment
    fprintf(stdout, "# blocknumber %d\n", blocknumber);
    fprintf(stdout, "# [0] %016lx\n", traceblock[0]);
    fprintf(stdout, "# [1] %s %02lx\n", 
            FormatUsecDateTime(traceblock[1] & 0x00fffffffffffffful),
            traceblock[1] >> 56);
    fprintf(stdout, 
            "# TS      DUR EVENT CPU PID RPC ARG0 RETVAL IPC NAME (t and dur multiples of 10ns)\n");

    if (verbose) {
       fprintf(stdout, "%% %02lx %014lx\n", traceblock[0] >> 56, traceblock[0] & 0x00fffffffffffffful);
       fprintf(stdout, "%% %02lx %014lx\n", traceblock[1] >> 56, traceblock[1] & 0x00fffffffffffffful);
    }
//   +-------+-----------------------+-------------------------------+
//   | cpu#  |                  cycle counter                        | 0 module
//   +-------+-----------------------+-------------------------------+
//   | flags |                  gettimeofday                         | 1 DoDump
//   +-------+-----------------------+-------------------------------+

    // traceblock[1] has flags in top byte. 
    uint8 flags = traceblock[1] >> 56;
    all_flags |= flags;
    bool this_block_has_ipc = (HasIPC(flags));

    // For each 64KB traceblock that has IPC_Flag set, also read the IPC bytes
    if (this_block_has_ipc) {
      // Extract 8KB IPC block
      int n = fread(ipcblock, 1, sizeof(ipcblock), f);
    } else {
      memset(ipcblock, 0, sizeof(ipcblock));	// Default if no IPC data
    }

// WRAPAROUND PROBLEM:
// We pick base_minute_usec here, but it can be
// long before the real wrapped trace entries in blocks 1..N
// Our downstream display does badly with seconds much over 120...
//
// We would like the base_minute_usec to be set by the first real entry in block 1 instead...
// Can still use paramaters here for basic time conversion. 
// Not much issue with overflow, I think.
//


    // If very first block, pick out time conversion parameters
    int first_real_entry = 2;
    bool very_first_block = (blocknumber == 0);
    if (very_first_block) {
      first_real_entry = 8;

      int64 start_cycles = traceblock[2];
      int64 start_usec = traceblock[3];
      int64 stop_cycles = traceblock[4];
      int64 stop_usec = traceblock[5];
      base_usec_timestamp = start_usec;

      if (verbose) {
        fprintf(stdout, "%% %016lx %ld cy %ld us (%ld)\n", traceblock[2], start_cycles, start_usec, start_usec % 60000000l);
        fprintf(stdout, "%% %016lx\n", traceblock[3]);
        fprintf(stdout, "%% %016lx %ld cy %ld us (%ld)\n", traceblock[4], stop_cycles, stop_usec, stop_usec % 60000000l);
        fprintf(stdout, "%% %016lx\n", traceblock[5]);
        fprintf(stdout, "%% %016lx unused\n", traceblock[6]);
        fprintf(stdout, "%% %016lx unused\n", traceblock[7]);
        fprintf(stdout, "\n");
      }


//   +-------------------------------+-------------------------------+
//   |                      start cycle counter                      | 2 DoDump
//   +-------------------------------+-------------------------------+
//   |                      start gettimeofday                       | 3 DoDump
//   +-------------------------------+-------------------------------+
//   |                       stop cycle counter                      | 4 DoDump
//   +-------------------------------+-------------------------------+
//   |                       stop gettimeofday                       | 5 DoDump
//   +-------------------------------+-------------------------------+
//   |                          u n u s e d                          | 6
//   +-------------------------------+-------------------------------+
//   |                          u n u s e d                          | 7
//   +-------------------------------+-------------------------------+

      uint64 block_0_cycle = traceblock[0] & 0x00fffffffffffffful;
      if ((block_0_cycle / start_cycles) > 1) {
        // Looks like bastard file: word 0 is unshifted by mistake
        unshifted_word_0 = true;
        first_real_entry = 6;
      }


      // Map start_cycles <==> start_usec
      SetParams(start_cycles, start_usec, stop_cycles, stop_usec, &params);

      // Round usec down to multiple of 1 minute
      base_minute_usec = (start_usec / 60000000) * 60000000;  
      // Backmap base_minute_usec to cycles
      base_minute_cycle = UsecToCycles(base_minute_usec, params);

      // Now instead map base_minute_cycle <==> 0
      SetParams10(base_minute_cycle, 0, &params);

      first_flags = flags;
//fprintf(stderr, "first_flags %02x\n", first_flags);
    }

    // Pick out CPU number for this traceblock
    current_cpu = traceblock[0] >> 56;
    unique_cpus.insert(current_cpu);	// stats

    // Pick out times for converting to 100Mhz
    uint64 base_cycle = traceblock[0] & 0x00fffffffffffffful;
    if (unshifted_word_0) {base_cycle >>= OLD_RDTSC_SHIFT;}
    uint64 prepend = base_cycle & ~0xfffff;

    // The base cycle count for this block may well be a bit later than the truncated time
    // in the first real entry, and may have wrapped in its low 20 bits. If so, the high bits 
    // we want to prepend should be one smaller.
    uint64 first_timestamp = traceblock[first_real_entry] >> 44;
    uint64 prior_t = first_timestamp;

    // If wraparound trace and in very_first_block, suppress everything except name entries
    bool keep_just_names = HasWraparound(first_flags) && very_first_block;

    if ((TracefileVersion(first_flags) >= 3) && !unshifted_word_0) {
      /* Every block has PID and pidname at the front */
      uint64 pid    = traceblock[first_real_entry + 0];
      uint64 unused = traceblock[first_real_entry + 1];
      char pidname[20];
      memcpy(pidname, reinterpret_cast<char*>(&traceblock[first_real_entry + 2]), 16);
      pidname[16] = '\0';

      if (verbose) {
        fprintf(stdout, "%% %016lx pid %ld\n", traceblock[first_real_entry + 0], pid);
        fprintf(stdout, "%% %016lx unused\n",  traceblock[first_real_entry + 1]);
        fprintf(stdout, "%% %016lx name %s\n", traceblock[first_real_entry + 2], pidname);
        fprintf(stdout, "%% %016lx name\n",    traceblock[first_real_entry + 3]);
        fprintf(stdout, "\n");
      }
// Every block has PID and pidname at the front
//   +-------------------------------+-------------------------------+
//   |           u n u s e d         |            PID                | 2 or 8  module
//   +-------------------------------+-------------------------------+
//   |                          u n u s e d                          | 3 or 9  module
//   +-------------------------------+-------------------------------+
//   |                                                               | 4 or 10 module
//   +                            pidname                            +
//   |                                                               | 5 or 11 module
//   +-------------------------------+-------------------------------+


      // Remember the name for this pid, except don't change pid 0
      uint64 nameinsert = PidToEvent(pid & 0xffff);
      if (nameinsert == 0x10000) {strcpy(pidname, kIdleName);}
      string name = string(pidname);
      names[nameinsert] = name;
      uint64 nsec10 = CyclesToNsec10(base_cycle, params);
      
      // To allow updates of the reconstruction stack in eventtospan
      OutputName(stdout, nsec10, nameinsert, name.c_str());

      // New user-mode process id, pid
      unique_pids.insert(pid);	// stats
      if (current_pid[current_cpu] != pid) {++ctx_switches;}	// stats
      current_pid[current_cpu] = pid;

      uint64 event = KUTRACE_USERPID;	// Context switch
      uint64 duration = 1;
      if (!keep_just_names) {
        name = AppendPid(name, pid);
        OutputEvent(stdout, nsec10, duration, event, current_cpu, 
                    pid, 0,  0, 0, 0, name.c_str());
        // Statistics: don't count as a context switch -- almost surely same
      }

      first_real_entry += 4;
    }


    // We wrapped if high bit of first_timestamp is 1 and high bit of base is 0
    if (Wrapped(first_timestamp, base_cycle)) {
      prepend -= 0x100000; 
      if (TRACEWRAP) {fprintf(stdout, "  Wrap0 %05lx %05lx\n", first_timestamp, base_cycle);}
    }

    for (int i = first_real_entry; i < kTraceBufSize; ++i) {
      uint8 ipc = ipcblock[i];

      // Completely skip any all-zero NOP entries
      if (traceblock[i] == 0) {continue;}

      // Skip the entire rest of the block if all-ones entry found
      if (traceblock[i] == 0xfffffffffffffffful) {break;}

      // +-------------------+-----------+---------------+-------+-------+
      // | timestamp         | event     | delta | retval|      arg0     |
      // +-------------------+-----------+---------------+-------+-------+
      //          20              12         8       8           16 
      
      uint64 t = traceblock[i] >> 44;			// Timestamp
      uint64 n = (traceblock[i] >> 32) & 0xfff;		// event number
      uint64 arg = traceblock[i] & 0x0000ffff;		// syscall/ret arg/retval
      uint64 delta_t = (traceblock[i] >> 24) & 0xff;	// Opt syscall return timestamp
      uint64 retval = (traceblock[i] >> 16) & 0xff;	// Opt syscall retval

      // Completely skip any mostly-FFFF entries
      if (n == 0xFFF) {continue;}

      // Sign extend optimized retval [-128..127] from 8 bits to 16
      retval = (uint64)(((int64)(retval << 56)) >> 56) & 0xffff;
      if (verbose) {
        fprintf(stdout, "%% %05lx %03lx %04lx %04lx = %ld %ld %ld.%ld %ld %02x\n", 
                (traceblock[i] >> 44) & 0xFFFFF, 
                (traceblock[i] >> 32) & 0xFFF, 
                (traceblock[i] >> 16) & 0xFFFF, 
                (traceblock[i] >> 0) & 0xFFFF, 
		t, n, delta_t, retval, arg, ipc);
      }

      if (is_mark(n)) {
        ++total_marks;	// stats
      } else {
        ++events_by_type[n >> 8];	// stats
      }

      uint64 event;
      if (n == KUTRACE_USERPID) {	// Context switch
        // New process id 64k+
        event = 0x10000 | arg;
      } else {
        // Anything else 0..64K-1
        event = n;
      }
      //FIXME
// 2019.03.18 Go back to preserving KUTRACE_USERPID for eventtospan
      event = n;

      // Convert truncated start time to full-width start time
      // Increment the prepend if truncated time rolls over
      if (Wrapped(prior_t, t)) {prepend += 0x100000;}
      prior_t = t;

      // tfull is increments of cycles from the base minute for this trace, 
      // also expressed as increments of cycles
      uint64 tfull = prepend | t;

      // nsec10 is increments of 10ns from the base minute.
      // For a trace starting at 50 seconds into a minute and spanning 99 seconds, 
      // this reaches 14,900,000,000 which means the 
      // base minute + 149.000 000 00 seconds. More than 32 bits.
      uint64 nsec10 = CyclesToNsec10(tfull, params);
      uint64 duration = 0;

      if ((n == KUTRACE_RPCIDREQ) || (n == KUTRACE_RPCIDMID)) {current_rpc[current_cpu] = arg;}
      if (n == KUTRACE_RPCIDRESP) {current_rpc[current_cpu] = 0;}

      // Pick out any name definitions 
      if (is_namedef(n)) {
        // We have a name or other variable-length entry
        uint64 nameinsert;
        if (is_pidnamedef(n)) {
          nameinsert = PidToEvent(arg & 0xffff);
        } else {
          nameinsert = ((n & 0x00f) << 8) | arg;
        }

        char tempstring[64];
        int len = (n >> 4) & 0x00f;
        if ((len < 1) || (8 < len)) {continue;}
        // Ignore any timepair
        if (!is_timepair(n)) {
          memset(tempstring, 0, 64);
          memcpy(tempstring, &traceblock[i + 1], (len - 1) * 8);
          // Remember the name for this pid, except don't change pid 0
          if (nameinsert == 0x10000) {strcpy(tempstring, kIdleName);}
          names[nameinsert] = string(tempstring);
          OutputName(stdout, nsec10, nameinsert, tempstring);
        }
        i += (len - 1);	// Skip over the rest of the name event
        continue;
      }
      
      if (keep_just_names) {continue;}

      //========================================================================
      // Name definitions above skip this code, so do not affect lo/hi 
      if (lo_timestamp > nsec10) {lo_timestamp = nsec10;}	// stats
      if (hi_timestamp < nsec10) {hi_timestamp = nsec10;}	// stats

      // Look for new user-mode process id, pid
      if (is_contextswitch(n)) {
        unique_pids.insert(arg);	// stats
        if (current_pid[current_cpu] != arg) {++ctx_switches;}	// stats
        current_pid[current_cpu] = arg;
      }

      // Nothing else, so dump in decimal
      // Here n is the original 12-bit event; event is (pid | 64K) if n is user-mode code
      string name = string("");

      // Put in name of event
      if (is_return(n)) {
        uint64 call_event = event & ~0x0200;
        if (names.find(call_event) != names.end()) {name.append("/" + names[call_event]);}
      } else {
        if (names.find(event) != names.end()) {name.append(names[event]);}
      }

      if (is_contextswitch(n)) {
        uint64 target = PidToEvent(arg & 0xffff);
        if (names.find(target) != names.end()) {name.append(names[target]);}
        name = AppendPid(name, arg);
     }

      if (is_usermode(event)) {
        if (names.find(event) != names.end()) {name.append(names[event]);}
        name = AppendPid(name, EventToPid(event));
      }

      // If this is an optimized call, pick out the duration and leave return value
      // The ipc value for this is two 4-bit values:
      //   low bits IPC before call, high bits IPC within call
      if (is_opt_call(n, delta_t)) {
        // Optimized call with delta_t and retval
        duration = CyclesToNsec10(tfull + delta_t, params) - nsec10;
        if (duration == 0) {duration = 1;}	// We enforce here a minimum duration of 10ns
      } else {
        retval = 0;
      }

      // If this is a special event marker, keep the name and arg
      if (is_special(n)) {
        name.append(string(kSpecialName[n & 0x000f]));
        arg = traceblock[i] & 0xffffffff;	// Use the full 32-bit argument
        if (has_rpcid(n)) {
          name = AppendPid(name, arg);
        }
        if (duration == 0) {duration = 1;}	// We enforce here a minimum duration of 10ns
      }

      // If this is an unoptimized return, move the arg value to retval
      if (is_return(n)) {
        retval = arg;
        arg = 0;
      }

      // If this is a call to an irq bottom half routine, name it
      if (is_bottom_half(n)) {
        name.append(":");
        name.append(string(soft_irq_name[arg & 0x000f]));
      }

      // MARK_A,B,C arg is six base-40 chars NUL, A_Z, 0-9, . - /
      // MARK_D     arg is unsigned int
      // +-------------------+-----------+-------------------------------+
      // | timestamp         | event     |              arg              |
      // +-------------------+-----------+-------------------------------+
      //          20              12                    32 

      if (is_mark_abc(n)) {
        // Include the marker label string
        name += "=";
        char temp[8];
        name += Base40ToChar(arg, temp);
      }

      // Output format:
      // time dur event cpu  pid rpc  arg retval IPC name(event)
      OutputEvent(stdout, nsec10, duration, event, current_cpu, 
                  current_pid[current_cpu], current_rpc[current_cpu], 
                  arg, retval, ipc, name.c_str());
      // Update some statistics
      ++event_count;	// stats
    }

   ++blocknumber;
  }	// while (fread...

  fclose(f);

  // Pass along the OR of all incoming raw traceblock flags, in particular IPC_Flag 
  fprintf(stdout, "# ## FLAGS: %d\n", all_flags);


  // Reduce timestamps to start at no more than 60 seconds after the base minute.
  // With wraparound tracing, we don't know the true value of lo_timestamp until
  // possibly the very last input block. So we offset here. The output file already 
  // has the larger times so eventtospan will reduce those. 
  uint64 extra_minutes = lo_timestamp / 6000000000l;
  uint64 offset_timestamp = extra_minutes * 6000000000l;
  lo_timestamp -= offset_timestamp;
  hi_timestamp -= offset_timestamp;
  double lo_seconds = lo_timestamp / 100000000.0;
  double hi_seconds = hi_timestamp / 100000000.0;
  double total_seconds = hi_seconds - lo_seconds;
  if (total_seconds <= 0.0) {
    lo_seconds = 0.0;
    hi_seconds = 1.0;
    total_seconds = 1.0;	// avoid zdiv
  }
  // Pass along the time bounds 
  fprintf(stdout, "# ## TIMES: %10.8f %10.8f\n", lo_seconds, hi_seconds);


  uint64 total_cpus = unique_cpus.size();
  if (total_cpus == 0) {total_cpus = 1;}	// avoid zdiv
 
  fprintf(stderr, "rawtoevent(%3.1fMB):\n", 
          blocknumber / kTraceBlocksPerMB); 
  fprintf(stderr, 
          "  %s,  %ld events, %ld CPUs  (%1.0f/sec/cpu)\n",
          FormatSecondsDateTime(base_usec_timestamp / 1000000),
          event_count, total_cpus, (event_count / total_seconds) /total_cpus); 
  uint64 total_irqs  = events_by_type[5] + events_by_type[7];
  uint64 total_traps = events_by_type[4] + events_by_type[6];
  uint64 total_sys64 = events_by_type[8] + events_by_type[9] +
                       events_by_type[10] + events_by_type[11];
  uint64 total_sys32 = events_by_type[12] + events_by_type[13] +
                       events_by_type[14] + events_by_type[15];
  uint64 total_other = events_by_type[0] + events_by_type[1] +
                       events_by_type[2] + events_by_type[3];
  fprintf(stderr, "  %ld IRQ, %ld Trap, %ld Sys64, %ld Sys32, %ld Mark, %ld Other\n",
          total_irqs, total_traps, total_sys64, total_sys32, total_marks, total_other); 
  fprintf(stderr, "  %ld PIDs, %ld context-switches (%1.0f/sec/cpu)\n", 
          unique_pids.size(), ctx_switches, (ctx_switches / total_seconds) / total_cpus);
  fprintf(stderr, 
          "  %5.3f elapsed seconds: %5.3f to %5.3f\n", 
          total_seconds, lo_seconds, hi_seconds); 

}

