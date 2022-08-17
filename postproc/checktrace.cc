// Little program to turn raw binary dclab trace files into Ascii event listings
// The main work is turning truncated cycle times into multiples of 10ns
// Copyright 2021 Richard L. Sites
//
// Input has filename like 
//   kutrace_control_20170821_095154_dclab-1_2056.trace
//
// compile with g++ -O2 checktrace.cc -o checktrace
//
// To see raw trace in hex, use kuod or
//   od -Ax -tx8z -w32 foo.trace
//
// dsites 2022.08.17 Initial version
//


#include <map>
#include <set>
#include <string>

#include <stdio.h>
#include <stdlib.h>     // exit
#include <string.h>
#include <time.h>
#include <unistd.h>     // getpid gethostname
#include <sys/stat.h>
#include <sys/time.h>   // gettimeofday
#include <sys/types.h>

#include "basetypes.h"
#include "kutrace_lib.h"

#define IPC_Flag     0x80
#define WRAP_Flag    0x40
#define Unused2_Flag 0x20
#define Unused1_Flag 0x10
#define VERSION_MASK 0x0F

using std::map;
using std::string;

typedef map<uint64, string> U64Name;	// Name for each syscall/PID/etc.

//VERYTEMP
bool tracenames = false;


static const int kTraceBufSize = 8192;	// uint64 count
static const int kIpcBufSize = 1024;	// uint64 count

// 2+ years in multiple of 10nsec
static const uint64 kMaxTimeCounter = 0x001FFFFFFFFFFFFFL;

// gettimeofday() 2016.01.01 * 1000000
static const uint64 kMinTimeOfDay = 1451606400000000L;

// gettimeofday() 2050.01.01 * 1000000
static const uint64 kMaxTimeOfDay = 2524608000000000L;

enum Err {
  WARN,
  FAIL,
  GOOD,		// Items below this are subpar
  INFO,
  NUM_ERR
};

const char* err_text[NUM_ERR] = {
  "Warn",
  "FAIL",
  "OK  ",
  "info",
};


enum Msg {
  // Overall trace
  TR_NOT_8K,
  TR_NOT_64K,
  TR_TRUNC,
  TR_TIME_HI,
  TR_TOD_LO,
  TR_TOD_HI,
  TR1_BACK_TC,
  TR1_BACK_TOD,
  TR1_FREQ_LO,
  TR1_FREQ_HI,
  TR1_UNUSED,
  TR1_RPI4,
  TR1_VERSION,
 
  TR_CALLSKEW,
  TR_NOTRAPS,
  TR_NOIRQS,
  TR_NOSYSCALLS,
  TR_NONAMES,
  TR_NOSWITCHES,
  TR_NOWAKEUPS,

  TR_NOPCSAMP,
  TR_NOFREQ,
  TR_NOLOPOWER,

  TR_OPTRPCS,
  TR_OPTLOCKS,
  TR_OPTQUEUES,
  TR_OPTMARKS,

  TR_NOKERNELVER,
  TR_KERNELVER,
  TR_NOMODEL,
  TR_MODEL,
  TR_NOHOST,
  TR_HOST,
  TR_BADCOUNT,
  TR_GOODCOUNT,
  TR_INFO,
  TR_RATIO,
  TR_EVENTS,	// At end of group; wrong text if edit error

  // First block header extra fields
  TR1_GOOD_1,

  // Each block header
  BH_CPU_HI,
  BH_UNUSED,
  BH_PID_HI,
  BH_FREQ_LO,
  BH_FREQ_HI,
  BH_ASCII,
  BH_TC_RANGE_LO,
  BH_TC_RANGE_HI,
  BH_TOD_RANGE_LO,
  BH_TOD_RANGE_HI,
  BH_TC_BACK,
  BH_TOD_BACK,

  // Block body
  BL_CROSS,
  BL_GOOD,	// At end of group wrong text if edit error

  NUM_MSG	// Must be last
};

const char* msg_text[NUM_MSG] = {
  "File size not multiple of 8KB:",
  "File size < 64KB:",
  "File is truncated",
  "Time counter is implausibly high:",
  "Time of day is before 2016:",
  "Time of day is after 2049:",
  "Start time counter > stop",
  "Start time of day > stop",
  "Apparent time counter increment < 25 MHz (>40ns):",
  "Apparent time counter increment > 100 MHz (<10ns):",
  "Unused bits are non-zero",
  "Skipping RPi4 time counter checks",
  "File version is not 3:",

  "Call:return ratio is skewed:",
  "Trace has no trap/fault events",
  "Trace has no interrupt events",
  "Trace has no syscall events",
  "Trace has no event names",
  "Trace has no context switches",
  "Trace has no wakeups",
  "Trace has no PC samples",
  "Trace has no frequency samples",
  "Trace has no low-power idle",

  "Trace has user-supplied RPCs",
  "Trace has user-supplied locks",
  "Trace has user-supplied queues",
  "Trace has user-supplied marks",

  "Trace has no kernel version",
  "Trace has kernel version:",
  "Trace has no model name",
  "Trace has model name:",
  "Trace has no host name",
  "Trace has host name:",
  "Trace has bad blocks:",
  "Trace has all good blocks:",
  "Trace has",
  "Trace call/return ratios are good",
  "Trace has no important missing events",

  "First block extra fields are good",

  "CPU number is >127:",
  "Unused bits are non-zero",
  "PID is high:",
  "CPU frequency is < 25 MHz:",
  "CPU frequency is > 9999 MHz:",
  "Not printable Ascii name:",
  "Time counter is before trace start",
  "Time counter is after trace stop",
  "Time of day is before trace start",
  "Time of day is after trace stop",
  "Time counter is before prior block",
  "Time of day is before prior block",

  "Event crosses block boundary, likely causing errors in prior block\n     ========",
  "is good",				// Block # prepended
};



//
// Globals
//
const char* fname = NULL;

bool trace_fail = false;
bool trace_warn = false;
bool verbose = false;
bool verbose_save = false;
bool hex = false;		// If set, print each entry in hex (debug)
bool quiet = false;		// If set, only give pass/fail line
bool nopf = false;		// Allow trace with no page faults to pass
size_t offset = 0;		// Byte offset of block start within file
int block_num = 0;		// Current Block number 0..n or -1 for no block
uint64 flags;			// Flags byte from first block of trace
bool skip_tc_checks = false;

uint64 start_time_counter = 0;
uint64 start_time_of_day  = 0;
uint64 stop_time_counter  = 0;
uint64 stop_time_of_day   = 0;
uint64 prior_time_counter = 0;
uint64 prior_time_of_day  = 0;

uint64 total_msg_count = 0;
uint64 total_block_count = 0;
uint64 total_bad_block_count = 0;

uint64 event_count[4096];
uint64 hasret_count[4096];
uint64 msg_count[NUM_MSG];

// Keep track of some peaks
int max_cpu;

uint64 peak_100msec;
uint64 peak_100msec_events;
uint64 current_100msec_events;
uint64 prior_100msec;

uint64 peak_second;
uint64 peak_second_events;
uint64 current_second_events;
uint64 prior_second;

uint64 peak_10second;
uint64 peak_10second_events;
uint64 current_10second_events;
uint64 prior_10second;

uint64 total_events_per_cpu[256];	// Events per CPU across the entire trace

U64Name names;

static const int kMaxDateTimeBuffer = 32;
static char gTempDateTimeBuffer[kMaxDateTimeBuffer];
static char gTempDateTimeBuffer2[kMaxDateTimeBuffer];
static const int kMaxPrintBuffer = 32;
static char gTempPrintBuffer[kMaxPrintBuffer];
static char gTempPrintBuffer2[kMaxPrintBuffer];


void Usage() {
  fprintf(stderr, "Usage: checktrace <filename> [-v] [-q] [-h] [-nopf]\n\n");
  fprintf(stderr, "       -v verbose, show hex at problem, more than two of each message\n");
  fprintf(stderr, "       -q quiet, just one line of PASS/FAIL output\n");
  fprintf(stderr, "       -h show hex for each event (debug)\n");
  fprintf(stderr, "       -nopf no page_fault checking, some files are OK without them\n");
  exit(0);
}

inline size_t size_min(size_t a, size_t b) {return (a < b) ? a : b;}
inline size_t size_max(size_t a, size_t b) {return (a > b) ? a : b;}

// Turn seconds since the epoch into date_hh:mm:ss
const char* FormatSecondsDateTimeBuff(uint64 sec, char* buffer) {
  if (sec == 0) {return "unknown";}  // Caller expecting date
  time_t tt = sec;
  struct tm* t = localtime(&tt);
  sprintf(buffer, "%04d-%02d-%02d_%02d:%02d:%02d", 
         t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, 
         t->tm_hour, t->tm_min, t->tm_sec);
  return buffer;
}

const char* FormatSecondsDateTime(uint64 sec) {
  return FormatSecondsDateTimeBuff(sec, gTempDateTimeBuffer);
}

const char* FormatSecondsDateTime2(uint64 sec) {
  return FormatSecondsDateTimeBuff(sec, gTempDateTimeBuffer2);
}


// Turn usec since the epoch into date_hh:mm:ss.usec
const char* FormatUsecDateTimeBuff(uint64 us, char* buffer) {
  if (us == 0) {return "unknown";}  // Longer spelling: caller expecting date
  uint64 seconds = us / 1000000;
  uint64 usec = us - (seconds * 1000000);
  char local_buffer[kMaxDateTimeBuffer];
  snprintf(buffer, kMaxPrintBuffer, "%s.%06lld", 
           FormatSecondsDateTimeBuff(seconds, local_buffer), usec);
  return buffer;
}

const char* FormatUsecDateTime(uint64 us) {
  return FormatUsecDateTimeBuff(us, gTempPrintBuffer);
}

const char* FormatUsecDateTime2(uint64 us) {
  return FormatUsecDateTimeBuff(us, gTempPrintBuffer2);
}

const char* FormatUint64(uint64 n) {
  snprintf(gTempPrintBuffer, kMaxPrintBuffer, "%llu", n);
  return gTempPrintBuffer;
}

const char* FormatUint642(uint64 n) {
  snprintf(gTempPrintBuffer2, kMaxPrintBuffer, "%llu", n);
  return gTempPrintBuffer2;
}

const char* FormatUint64x(uint64 n) {
  snprintf(gTempPrintBuffer, kMaxPrintBuffer, "0x%llx", n);
  return gTempPrintBuffer;
}

const char* FormatUint64x2(uint64 n) {
  snprintf(gTempPrintBuffer2, kMaxPrintBuffer, "0x%llx", n);
  return gTempPrintBuffer2;
}

uint8 GetFlags(uint64* traceblock) {
  return (uint8)((traceblock[1] >> 56) & 0xFF);
}

bool HasIPC(uint8 flags) {
  return (flags & IPC_Flag);
}
bool HasWrap(uint8 flags) {
  return (flags & WRAP_Flag);
}

bool IsVarLen(uint64 event) {
  // Skip the Historical mistakes
  if (event == KUTRACE_PC_TEMP) {return false;}
  if (event == KUTRACE_PC_U) {return false;}
  if (event == KUTRACE_PC_K) {return false;}
  // Variable-length names
  if ((KUTRACE_VARLENLO <= event) && (event <= KUTRACE_VARLENHI)) {
    return true;
  } 
  return false;
}

// Most events are 1 word; a few are longer
int GetEventLen(uint64 event) {
  // Historical mistakes
  if (event == KUTRACE_PC_TEMP) {return 2;}
  if (event == KUTRACE_PC_U) {return 2;}
  if (event == KUTRACE_PC_K) {return 2;}
  // Variable-length names
  if ((KUTRACE_VARLENLO <= event) && (event <= KUTRACE_VARLENHI)) {
    int possible_len = (event >> 4) & 0x00F;
    return possible_len ? possible_len : 1;	// Never return zero length
  } 
  return 1;
}

// Remove the length bits from an event number
inline uint64 NoLen(uint64 e) {return e & 0xF0F;}

// Key for name insert
// The event number is 001 through 1FF, specifying what is named,
// and arg0 is which specific item is being named. For example,
// event=028 arg0=000F is naming a syscall, number 15 (chmod in Linux)
// The last hex digit of the naming event number corresponds to the first 
// hex digit for most events that are named, e.g. naming event 80F.
inline uint64 MakeKey(uint64 event, uint64 arg0) {
  return (NoLen(event) << 16) | arg0;
}

// Some variable-length name entries hove no itme number in arg0
bool HasNoItemNum(uint64 event) {
  if (event == KUTRACE_TIMEPAIR) {return true;}
  if (event == KUTRACE_PC_TEMP) {return true;}
  if (event == KUTRACE_KERNEL_VER) {return true;}
  if (event == KUTRACE_MODEL_NAME) {return true;}
  if (event == KUTRACE_HOST_NAME) {return true;}
  return false;
}

// Key for name lookup, preferred form
// including 64-bit with more than 512 syscalls
uint64 MakeKeyFromEvent(uint64 event) {
  // Bare events are just event as the high part of the key
  if (HasNoItemNum(event)) {
    return event << 16;
  }

  // Keys for many names are in groups of 256, but keys for system calls
  // are in groups of 512.
  if ((0x800 <= event) && (event <= 0xBFF)) {
    return (0x008 << 16) | (event & 0x1FF);
  } else if ((0xC00 <= event) && (event <= 0xFFF)) {
    // Preferred form -- 64-bit with more than 512 syscalls
    return (0x008 << 16) | (event & 0x1FF | 0x400) ;
  } else {
    // Traps, Interrupts
    return (((event & 0xF00) >> 8) << 16) | (event & 0x0FF);
  }
}

// Key for name lookup, alternate form
// including 32-bit with real syscall32 name
uint64 MakeKeyFromEventAlt(uint64 event) {
  // Bare events are just event as the high part of the key
  if (HasNoItemNum(event)) {
    return event << 16;
  }

  // Keys for many names are in groups of 256, but keys for system calls
  // are in groups of 512.
  if ((0x800 <= event) && (event <= 0xBFF)) {
    return (0x008 << 16) | (event & 0x1FF);
  } else if ((0xC00 <= event) && (event <= 0xFFF)) {
    // Alternate form -- 32-bit with actual syscall32 name
    return (0x00C << 16) | (event & 0x1FF) ;
   } else {
    // Traps, Interrupts
    return (((event & 0xF00) >> 8) << 16) | (event & 0x0FF);
  }
}

// Force all printable Ascii 
void CleanupAscii(char* str, int len) {
  for (int i = 0; i < len; ++i) {
    char c = str[i];
    if (c == '\0') {break;}
    if ((c < 0x20) || (0x7E < c)) {str[i] = '_';}
  }
}


// If variable-length entry (name), remember it using key calculation
//  event_no_len = event &0xF0F (middle four bits are entry length)
//  key of (event_no_len << 16) | arg0 
// where event says what kind of name, and arg0 says which item is named
void SaveName(uint64 event, uint64 arg0, int event_len, uint64* traceblock_i) {
  char nametemp[64];
  int namelen = (event_len - 1) * 8;	// Eight bytes per name word
  if (namelen <= 0) {return;}		// Avoid core dump on bogus length
  if (namelen > 56) {
    namelen = 56;
  }
  memcpy(nametemp, (char*)(&traceblock_i[1]), namelen);
  nametemp[namelen] = '\0';
  CleanupAscii(nametemp, namelen);
  uint64 key = MakeKey(event, arg0);
  names[key] = string(nametemp);
if (tracenames) fprintf(stdout, "%016llx insert names[%07llx] %s\n", *traceblock_i, key, nametemp);
}

inline bool IsCall(uint64 event) {
  return ((KUTRACE_TRAP <= event) && ((event & 0x0200) == 0));
}

inline bool IsCallRet(uint64 event, uint64 delta_t) {
  return (delta_t > 0) && IsCall(event);
}


// Print line of hex stating at a multipfle of 64 bytes 
void PrintHex(size_t delta_byte, uint64* block) {
  if (block == NULL) {return;}
  // 32 bytes per line
  size_t line_start_byte = (offset + delta_byte) & ~0x01f;  // In file, bytes
  size_t line_start_subscr = (delta_byte & ~0x01f) >> 3;    // In block, uint64
  fprintf(stdout, "     [%06lx] ", line_start_byte);
  for (int j = 0; j < 4; ++j) {
    fprintf(stdout, "%016llx  ", block[line_start_subscr + j]);
  }
  fprintf(stdout, "\n");
  // Print carets under bad byte
  int spaces = 14;				// The inital [%06lx]
  spaces += ((delta_byte & 0x1f) / 8) * 2;	// Trailing spaces per word
  spaces += (delta_byte & 0x1f) * 2;		// Two hex per byte
  for (int i = 0; i < spaces; ++i) {fprintf(stdout, " ");}
  fprintf(stdout, "^^\n");
}

// Always return subpar true for fail/warn
bool Note(Err err, Msg msg, uint64* block, size_t delta_byte, const char* str) {
  trace_fail |= (err == FAIL);
  trace_warn |= (err == WARN);
  bool subpar = (err < GOOD);
  ++msg_count[msg];
  ++total_msg_count;
  if ((!verbose) && (2 < msg_count[msg])) {return subpar;}  // At most twice per message number
  if (quiet) {return subpar;}

  if (0 <= block_num) {
    fprintf(stdout, "%s Block %d %s %s\n", err_text[err], block_num, msg_text[msg], str);
    if (verbose && (block != NULL)) {
      // Print aligned line of four entries including offset
      PrintHex(delta_byte, block);
    }
  } else {
    fprintf(stdout, "%s %s %s\n", err_text[err], msg_text[msg], str);
  }

  if (verbose && (20 == total_msg_count)) {
    fprintf(stdout, "    More verbose messages suppressed\n\n");
    verbose_save = verbose;
    verbose = false;
  }

  return subpar;
}

bool Note2(Err err, Msg msg, uint64* block, size_t delta_byte, const char* str, const char* str2) {
  bool subpar = Note(err, msg, block, delta_byte, str);
  if ((!verbose) && (2 < msg_count[msg])) {return subpar;}  // At most twice per message number
  if (quiet) {return subpar;}
  fprintf(stdout, "     Actual value: %s\n", str2);
  return subpar;
}


// These tests fail immediately
FILE* CheckStat(const char* fname) {
  if (fname == NULL) {Usage();}
  struct stat buff;
  int status = stat(fname, &buff);
  if (status < 0) {
    fprintf(stdout, "FAILFAST %s %s\n\n", "NO FILE", fname);
    exit(0);
  }

  FILE* f = fopen(fname, "rb");
  if (f == NULL) {
    // Should never happen since CheckStat already tested
    fprintf(stdout, "FAILFAST %s %s\n\n", "NO FILE", fname);
    exit(0);
  }

  bool fail_fast = false;
  // Size must be a multiple of 8KB >= 64KB (blocks are 64KB or 72KB)
  if ((buff.st_size & 0x1FFF) != 0) {
    fail_fast |= Note(FAIL, TR_NOT_8K, NULL, 0, FormatUint64x(buff.st_size));
  }
  if (buff.st_size < 64 * 1024) {
    fail_fast |=Note(FAIL, TR_NOT_64K, NULL, 0, FormatUint64(buff.st_size));
  }

  if (fail_fast) {
    fprintf(stdout, "FAILFAST %s %s \n\n", "NOT 8K MULTIPLE OR TOO SMALL", fname);
    exit(0);
  }

  // If good, return the open file
  return f;
}

// Return true if subpar -- fail or warn
bool CheckTimePair(uint64 time_counter, uint64 time_of_day, uint64* traceblock, uint64 byte_offset) {
  bool subpar = false;
  if (!skip_tc_checks) {
    if (kMaxTimeCounter < time_counter) {
      subpar |= Note(FAIL, TR_TIME_HI, traceblock, byte_offset, FormatUint64x(time_counter));
    }
  }
  if (time_of_day < kMinTimeOfDay) {
    subpar |= Note(FAIL, TR_TOD_LO, traceblock, byte_offset + 8, FormatUsecDateTime(time_of_day));
  }
  if (kMaxTimeOfDay < time_of_day) {
    subpar |= Note(FAIL, TR_TOD_HI, traceblock, byte_offset + 8, FormatUsecDateTime(time_of_day));
  }
  return subpar;
}


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
//   +===============================+===============================+

// Return true if subpar -- fail or warn
bool CheckFirstTraceBlock(uint64* traceblock) {
  bool subpar = false;
  start_time_counter = traceblock[2];
  start_time_of_day  = traceblock[3];
  stop_time_counter  = traceblock[4];
  stop_time_of_day   = traceblock[5];

  // RPi4 time counter is only 32 bits, so wraps every 76 seconds @ 54MHz
  // We skip the time counter checks for it.
  // We can tell it is RPi4 because the start and stop time counters fit 
  //  into 32 bits (on other machines, they won't, at least soon after booting).
  skip_tc_checks = ((start_time_counter | stop_time_counter) & ~0xFFFFFFFFL) == 0;
  if (skip_tc_checks) {
    Note(INFO,  TR1_RPI4, traceblock, 2*8, "");
  }

  // Check full-trace start and stop times for plausibility
  subpar |= CheckTimePair(start_time_counter, start_time_of_day, traceblock,  2*8);
  subpar |= CheckTimePair(stop_time_counter, stop_time_of_day, traceblock, 4*8);

  // Check that stop is strictly after start
  if (!skip_tc_checks) {
    if (start_time_counter >= stop_time_counter) {
      subpar |= Note(FAIL, TR1_BACK_TC, traceblock, 2*8, "");
    }
  }
  if (start_time_of_day >= stop_time_of_day) {
    subpar |= Note(FAIL, TR1_BACK_TOD, traceblock, 3*8, "");
  }

  // Check apparent time counter frequency
  uint64 elapsed_time_counter = stop_time_counter - start_time_counter;
  uint64 elapsed_time_of_day = stop_time_of_day - start_time_of_day;      //usec
  // Only check frequency if times appear to be good so far
  if (!subpar && !skip_tc_checks) {
    uint64 counts_per_usec = elapsed_time_counter / elapsed_time_of_day;  //MHz
    if (counts_per_usec < 25) {
      snprintf(gTempPrintBuffer2, kMaxPrintBuffer, "%llutc / %lluus", elapsed_time_counter, elapsed_time_of_day);
      subpar |= Note2(WARN, TR1_FREQ_LO, traceblock, 2*8, FormatUint64(counts_per_usec), gTempPrintBuffer2);
    }
    if (100 < counts_per_usec) {
      snprintf(gTempPrintBuffer2, kMaxPrintBuffer, "%llutc / %lluus", elapsed_time_counter, elapsed_time_of_day);
      subpar |= Note2(WARN, TR1_FREQ_HI, traceblock, 2*8, FormatUint64(counts_per_usec), gTempPrintBuffer2);
    }

  }

  // Warn if the unused words become used
  if ((traceblock[6] != 0) || (traceblock[7] != 0)) {
    subpar |= Note(WARN, TR1_UNUSED, traceblock, 6*8, "");
  }

  // Trace-format version number is only in the first block's flags
  flags = GetFlags(traceblock);
  if ((flags & VERSION_MASK) != 3) {
    subpar |= Note(WARN, TR1_VERSION, traceblock, 1*8, FormatUint64(flags & VERSION_MASK));
  }

  // Fail fast on zero version -- trace file is too old
  if ((flags & VERSION_MASK) < 3) {
    fprintf(stdout, "FAILFAST %s %s\n\n", "Too-old trace version", fname);
    exit(0);
  }

  if (!subpar) {Note(GOOD, TR1_GOOD_1, NULL, 0, "");}
  return subpar;
}

// Check for printable Ascii
bool CheckAscii(uint64* traceblock, int entry, int len) {
  uint8* base = (uint8*)&traceblock[entry];
  bool any_bad = false;
  if (64 < len) {len = 64;}
  if (len < 0) {len = 0;}
  for (int i = 0; i < len; ++i) {
    uint8 c = base[i];
    if (c == '\0') {break;}
    if ((0x20 <= c) && (c <= 0x7E)) {continue;}
    any_bad = true;
  }
  if (any_bad) {
    char temp[80];
    int k = 0;
    temp[k++] = '\'';
    if (16 < len) {len = 16;}
    for (int i = 0; i < len; ++i) {
      uint8 c = base[i];
      if ((0x20 <= c) && (c <= 0x7E)) {
        temp[k++] = c;
        temp[k++] = ' ';
       } else {
        temp[k++] = "0123456789ABCDEF"[c>>4];
        temp[k++] = "0123456789ABCDEF"[c&15];
      }
      temp[k++] = ' ';
    }
    temp[k++] = '\'';
    temp[k++] = '\0';
    Note(WARN, BH_ASCII, traceblock, entry*8, temp);
  }
  return any_bad;
}


//   +-------+-----------------------+-------------------------------+
//   | cpu#  |                  cycle counter                        | 0 module
//   +-------+-----------------------+-------------------------------+
//   | flags |                  gettimeofday                         | 1 DoDump
//   +===============================+===============================+
//   |           (freq)              |            PID                | 2 or 8 module
//   +-------------------------------+-------------------------------+
//   |                          u n u s e d                          | 3 or 9 module
//   +-------------------------------+-------------------------------+
//   |                                                               | 4 or 10 module
//   +                            pidname                            +
//   |                                                               | 5 or 11 module
//   +-------------------------------+-------------------------------+

// Return true if subpar -- fail or warn
bool CheckBlockHeader(uint64* traceblock, int next_entry) {
  bool subpar = false;
  uint64 cpu = traceblock[0] >> 56;
  uint64 time_counter = traceblock[0] & 0x00FFFFFFFFFFFFFFL;
  uint64 block_flags = traceblock[1] >> 56;
  uint64 time_of_day = traceblock[1] & 0x00FFFFFFFFFFFFFFL;

  if (max_cpu < cpu) {
    max_cpu = cpu;
  }
  // Warn if the CPU number is unexpectedly large
  if (127 < cpu) {
    subpar |= Note(WARN, BH_CPU_HI, traceblock, 0*8, FormatUint64(cpu));
  }
  // Warn if the unused flag bits become used
  if ((block_flags & (Unused2_Flag | Unused1_Flag)) != 0) {
    subpar |= Note(WARN, BH_UNUSED, traceblock, 1*8, FormatUint64x(block_flags & ~VERSION_MASK));
  }

  // Check block start and stop times for plausibility
  subpar |= CheckTimePair(time_counter, time_of_day, traceblock, 0*8);

  // Check that this block is within overall trace time range
  if (!skip_tc_checks) {
    if (time_counter < start_time_counter) {
      subpar |= Note2(FAIL, BH_TC_RANGE_LO, traceblock, 0*8, 
        FormatUint64x(start_time_counter), FormatUint64x2(time_counter));
    }
    if (stop_time_counter < time_counter) {
      subpar |= Note2(FAIL, BH_TC_RANGE_HI, traceblock, 0*8, 
        FormatUint64x(stop_time_counter), FormatUint64x2(time_counter));
    }
  }
  if (time_of_day < start_time_of_day) {
    subpar |= Note2(FAIL, BH_TOD_RANGE_LO, traceblock, 1*8, 
      FormatUsecDateTime(start_time_of_day), FormatUsecDateTime2(time_of_day));
  }
  if (stop_time_of_day < time_of_day) {
    subpar |= Note2(FAIL, BH_TOD_RANGE_HI, traceblock, 1*8, 
      FormatUsecDateTime(stop_time_of_day), FormatUsecDateTime2(time_of_day));
  }

  // Check that this block is after previous block
  if (!skip_tc_checks) {
    if (time_counter < prior_time_counter) {
      subpar |= Note2(FAIL, BH_TC_BACK, traceblock, 0*8, 
        FormatUint64x(prior_time_counter), FormatUint64x2(time_counter));
    }
  }
  if (time_of_day < prior_time_of_day) {
    subpar |= Note2(FAIL, BH_TOD_BACK, traceblock, 1*8, 
      FormatUsecDateTime(prior_time_counter), FormatUsecDateTime2(time_counter));
  }
  prior_time_counter = time_counter;
  prior_time_of_day = time_of_day;

  // Check block-start PID and possible frequency
  uint64 pid = traceblock[next_entry] & 0x00000000FFFFFFFFL;
  uint64 freq = traceblock[next_entry] >> 32;
  uint64 unused = traceblock[next_entry + 1];

  // PID should fit in 20 bits (16 except FreeBSD)
  if ((pid & 0xFFF00000) != 0) {
    subpar |= Note(WARN, BH_PID_HI, traceblock, next_entry*8, FormatUint64(pid));
  }

  if ((freq != 0) && (freq < 25)) {
    subpar |= Note(WARN, BH_FREQ_LO, traceblock, next_entry*8, FormatUint64(freq));
  }
  if ((freq != 0) && (9999 < freq)) {
    subpar |= Note(WARN, BH_FREQ_HI, traceblock, next_entry*8, FormatUint64(freq));
  }

  if (unused != 0) {
    subpar |= Note(WARN, BH_UNUSED, traceblock, (next_entry + 1)*8, "");
  }

  subpar |= CheckAscii(traceblock, next_entry + 2, 16);

  return subpar;
}


// Determine whether ts1 is before ts2
// Timestamps may wrap, leaving ts1 much smaller than ts2
// and may go slightly backward without harm
// We allow "slightly" to mean  4096 counts smaller (40-80 usec)
bool TsBefore() {
  return false;
}

      // +-------------------+-----------+---------------+-------+-------+
      // | timestamp         | event     | delta | retval|      arg0     |
      // +-------------------+-----------+---------------+-------+-------+
      //          20              12         8       8           16 

// Return true if subpar -- fail or warn
bool CheckBlockBody(uint64* traceblock, int next_entry, int* block_events) {
  bool subpar = false;
  uint64 block_event_count = 0;

  for (int i = next_entry; i < kTraceBufSize; ++i) {
    if (hex) {fprintf(stdout, "[%4d] %016llx\n", i, traceblock[i]);}
    uint64 ts = (traceblock[i] >> 44) & 0xFFFFF;
    uint64 event = (traceblock[i] >> 32) & 0xFFF;
    uint64 delta_t = (traceblock[i] >> 24) & 0xFF;
    uint64 retval = (traceblock[i] >> 16) & 0xFF;
    uint64 arg0 = (traceblock[i] >> 0) & 0xFFFF;
    int event_len = GetEventLen(event);

    // Count all events, and also any optimized returns
    ++event_count[event];
    ++block_event_count;
    if (IsCallRet(event, delta_t)) {
      ++hasret_count[event];
      ++block_event_count;
    }


    // Can't do this test reliably with 20-bit wraparound if we allow almost  
    //   all counts delta to be good
    // Check for monotonic time
    //if (TsBefore(ts, prior_ts)) {
    //  subpar |= Note(FAIL, BL_BACK, traceblock, i*8, "");
    //}

    // If variable-length entry (name), remember it 
    if (IsVarLen(event)) {
      SaveName(event, arg0, event_len, &traceblock[i]);
    }

    // Extra advance over multi-word events
    if (1 < event_len) {
      i += (event_len - 1);
      // Check for block overflow
      if (kTraceBufSize <= i) {
        subpar |= Note(FAIL, BL_CROSS, traceblock, i*8, "");
      }
    }

    // Keep track of the event counts per CPU and per second
    *block_events = block_event_count;
  }

  return subpar;
}

void TrackBlockEvents(uint64* traceblock, int block_events) {
  uint64 cpu = traceblock[0] >> 56;
  uint64 time_of_day = traceblock[1] & 0x00FFFFFFFFFFFFFFL;
  uint64 current_100msec =  time_of_day / 100000;
  uint64 current_second =   time_of_day / 1000000;
  uint64 current_10second = time_of_day / 10000000;
  
  // Find peak 1/10 second: events across all CPUs
  if (prior_100msec != current_100msec) {
    if (peak_100msec_events < current_100msec_events) {
      peak_100msec_events = current_100msec_events;
      peak_100msec = prior_100msec;
    }
    prior_100msec = current_100msec;
    current_100msec_events = 0;
  }
  // Find peak second: events across all CPUs
  if (prior_second != current_second) {
    if (peak_second_events < current_second_events) {
      peak_second_events = current_second_events;
      peak_second = prior_second;
    }
    prior_second = current_second;
    current_second_events = 0;
  }
  // Find peak 10second: events across all CPUs
  if (prior_10second != current_10second) {
    if (peak_10second_events < current_10second_events) {
      peak_10second_events = current_10second_events;
      peak_10second = prior_10second;
    }
    prior_10second = current_10second;
    current_10second_events = 0;
  }

  current_100msec_events += block_events;
  current_second_events += block_events;
  current_10second_events += block_events;

  // Per-CPU events across entire trace
  total_events_per_cpu[cpu] += block_events;
}

// Called after all blocks, to handle peaks at end of trace
void FinishBlockEvents() {
  // Find peak 1/10 second: events across all CPUs
  if (peak_100msec_events < current_100msec_events) {
    peak_100msec_events = current_100msec_events;
    peak_100msec = prior_100msec;
  }

  // Find peak second: events across all CPUs
  if (peak_second_events < current_second_events) {
    peak_second_events = current_second_events;
    peak_second = prior_second;
  }

  // Find peak 10second: events across all CPUs
  if (peak_10second_events < current_10second_events) {
    peak_10second_events = current_10second_events;
    peak_10second = prior_10second;
  }
}

// Return true if subpar -- fail or warn
bool CheckTraceBlock(size_t n, uint64* traceblock) {
  bool subpar = false;
  int block_events;
  // Must be 64KB
  if ((n & 0xFFFF) != 0) {
    subpar |= Note(FAIL, TR_TRUNC, traceblock, 0, "");
    subpar = true;
  }

  // First block extra checks
  // Sets global time range, so must be before calling CheckBlockHeader 
  int next_entry = 2;
  if (block_num == 0) {
    subpar |= CheckFirstTraceBlock(traceblock);
    next_entry = 8;
  }

  subpar |= CheckBlockHeader(traceblock, next_entry);
  next_entry += 4;					// Over the PID #,name
  subpar |= CheckBlockBody(traceblock, next_entry, &block_events);

  TrackBlockEvents(traceblock, block_events);

  // Give overall blessing for positive feedback
  if (!subpar) {Note(GOOD, BL_GOOD, NULL, 0, "");}
  return subpar;
}

// Return true if subpar -- fail or warn
bool CheckIpcBlock(size_t n, uint64* ipcblock) {
  bool subpar = false;
  // Must be 8KB
  if ((n & 0xFFF) != 0) {
    subpar |= Note(FAIL, TR_TRUNC, ipcblock, 0, "");
  }
  // Only other test I can think of is to see if density of 1-bits is about 
  // the same in both 32-bit halves. Left density is higher in trace blocks.
  return subpar;
}

// Get a name for event number 000 .. FFF
const char* GetEventName(int event) {
  const char* retval;
  uint64 key = MakeKeyFromEvent(event);
  if (names.find(key) == names.end()) {
    // The preferred form is not found. Try the alternate form.
    key = MakeKeyFromEventAlt(event);
    if (names.find(key) == names.end()) {
      // The alternate form is not found. Give the event number
      snprintf(gTempPrintBuffer2, kMaxPrintBuffer, "sys#%03x", event);
      retval = gTempPrintBuffer2;
    } else {
      retval = names[key].c_str();
    }
  } else { 
    retval = names[key].c_str();
  }
if (tracenames) fprintf(stdout, "get names[%07llx] %s\n", key,  retval);
  return retval;
}

// Return true if subpar -- fail or warn
bool CheckEventCounts() {
  bool subpar = false;

  // Aggregate across blocks of 256 event numbers
  uint64 pergroup_count[16];	
  uint64 perret_count[16];
  memset(pergroup_count, 0, 16 * sizeof(uint64));
  memset(perret_count, 0, 16 * sizeof(uint64));
  for (int i = 0; i <= 0xFFF; ++i) {
    pergroup_count[i >> 8] += event_count[i];	// Groups of 256
    perret_count[i >> 8] += hasret_count[i];
  }

  // Aggregate within names 0x0 to 1xF for x!=0
  for (int i = KUTRACE_VARLENLO; i < KUTRACE_VARLENHI; ++i) {
    if ((i & 0x0F0) == 0) {continue;}
    event_count[NoLen(i)] += event_count[i];
  }

  // Look for missing events
  // FAIL if these are missing
  if (!nopf && (pergroup_count[4] + pergroup_count[6]) == 0) {
    subpar |= Note(FAIL, TR_NOTRAPS, NULL, 0, "");
  }
  if ((pergroup_count[5] + pergroup_count[7]) == 0) {
    subpar |= Note(FAIL, TR_NOIRQS, NULL, 0, "");
  }
  if ((pergroup_count[8] + pergroup_count[9] +
       pergroup_count[10] + pergroup_count[11] +
       pergroup_count[12] + pergroup_count[13] +
       pergroup_count[14] + pergroup_count[15]) == 0) {
    subpar |= Note(FAIL, TR_NOSYSCALLS, NULL, 0, "");
  }
  if ((pergroup_count[0] + event_count[0]) == 0) {	//Ignore NOPs
    subpar |= Note(FAIL, TR_NONAMES, NULL, 0, "");
  }
  if (event_count[KUTRACE_USERPID] == 0) {	
    subpar |= Note(FAIL, TR_NOSWITCHES, NULL, 0, "");
  }
  // Clone and wait4 can transition without an explicit wakeup, so just warn
  if (event_count[KUTRACE_RUNNABLE] == 0) {	
    subpar |= Note(WARN, TR_NOWAKEUPS, NULL, 0, "");
  }

  if (!subpar) {Note(GOOD, TR_EVENTS, NULL, 0, "");}

  // Look for event correlations
  // Check for balanced call vs. return
  // Return event is call + 0x200
  for (int i = KUTRACE_TRAP; i <= 0xFFF; ++i) {
    if (i & 0x200) {continue;}		// Skip over all returns
    uint64 calls = event_count[i];
    uint64 rets = hasret_count[i] + event_count[i + 0x200];
    uint64 sum = calls + rets;
    // Ignore small counts
    if (10 <= calls) {
      int callper = (calls * 100) / sum;
      int retper = 100 - callper;
      // Ignore nearly 50:50 ratio
      if (55 < callper) {	// Complain over 55:45 or worse
        char temp[128];
        const char* this_name = GetEventName(i);
        sprintf(temp, "%s %llu:%llu", this_name, calls, rets);
        subpar |= Note(WARN, TR_CALLSKEW, NULL, 0, temp);
      }
    }
  }

  if (!subpar) {Note(GOOD, TR_RATIO, NULL, 0, "");}


  // Check trace-context entries
  if (event_count[KUTRACE_KERNEL_VER] == 0) {	
    subpar |= Note(WARN, TR_NOKERNELVER, NULL, 0, "");
  } else {
    uint64 key = MakeKeyFromEvent(KUTRACE_KERNEL_VER);
    subpar |= Note(INFO, TR_KERNELVER, NULL, 0, names[key].c_str());
  }

  if (event_count[KUTRACE_MODEL_NAME] == 0) {	
    subpar |= Note(INFO, TR_NOMODEL, NULL, 0, "");
  } else {
    uint64 key = MakeKeyFromEvent(KUTRACE_MODEL_NAME);
    subpar |= Note(INFO, TR_MODEL, NULL, 0, names[key].c_str());
  }

  if (event_count[KUTRACE_HOST_NAME] == 0) {	
    subpar |= Note(INFO, TR_NOHOST, NULL, 0, "");
  } else {
    uint64 key = MakeKeyFromEvent(KUTRACE_HOST_NAME);
    subpar |= Note(INFO, TR_HOST, NULL, 0, names[key].c_str());
  }

  // INFO if these are missing; do not set subpar
  if ((event_count[KUTRACE_PC_TEMP] + 
       event_count[KUTRACE_PC_U] + event_count[KUTRACE_PC_K]) == 0) {	
    subpar |= Note(INFO, TR_NOPCSAMP, NULL, 0, "");
  }

  if ((event_count[KUTRACE_PSTATE] + event_count[KUTRACE_PSTATE2]) == 0) {	
    subpar |= Note(INFO, TR_NOFREQ, NULL, 0, "");
  }

  if (event_count[KUTRACE_MWAIT] == 0) {	
    subpar |= Note(INFO, TR_NOLOPOWER, NULL, 0, "");
  }

  // User software supplied events
  if ((event_count[KUTRACE_RPCIDREQ] + event_count[KUTRACE_RPCIDRESP] +
       event_count[KUTRACE_RPCIDMID] + event_count[KUTRACE_RPCIDRXMSG] +
       event_count[KUTRACE_RPCIDTXMSG]) != 0) {	
    subpar |= Note(INFO, TR_OPTRPCS, NULL, 0, "");
  }

  if ((event_count[KUTRACE_LOCKNOACQUIRE] + event_count[KUTRACE_LOCKACQUIRE] +
       event_count[KUTRACE_LOCKWAKEUP]) != 0) {	
    subpar |= Note(INFO, TR_OPTLOCKS, NULL, 0, "");
  }

  if ((event_count[KUTRACE_ENQUEUE] + event_count[KUTRACE_DEQUEUE]) != 0) {	
    subpar |= Note(INFO, TR_OPTQUEUES, NULL, 0, "");
  }

  if ((event_count[KUTRACE_MARKA] + event_count[KUTRACE_MARKB] +
       event_count[KUTRACE_MARKC] + event_count[KUTRACE_MARKD]) != 0) {	
    subpar |= Note(INFO, TR_OPTMARKS, NULL, 0, "");
  }

  return subpar;
}


int main (int argc, const char** argv) {
  //const char* fname = NULL;

  // Arguments
  for (int i = 1; i < argc; ++i) {
    if (argv[i][0] != '-') {
      fname = argv[i];
    } else if (strcmp(argv[i], "-v") == 0) {
      verbose = true;
    } else if (strcmp(argv[i], "-q") == 0) {
      quiet = true;
    } else if (strcmp(argv[i], "-h") == 0) {
      hex = true;
    } else if (strcmp(argv[i], "-nopf") == 0) {
      nopf = true;
    } else {
      Usage();
    }
  }

  // Initialize
  memset(event_count, 0, 4096 * sizeof(uint64));
  memset(hasret_count, 0, 4096 * sizeof(uint64));
  memset(msg_count, 0, NUM_MSG * sizeof(uint64));
  
  max_cpu = 0;

  peak_100msec = 0;
  peak_100msec_events = 0;
  current_100msec_events = 0;
  prior_100msec = 0;
  peak_second = 0;
  peak_second_events = 0;
  current_second_events = 0;
  prior_second = 0;
  peak_10second = 0;
  peak_10second_events = 0;
  current_10second_events = 0;
  prior_10second = 0;

  memset(total_events_per_cpu, 0, 256 * sizeof(uint64));


  // Exits if any problem with file -- fail_fast
  FILE* f = CheckStat(fname);

  // Loop reading and testing trace blocks
  uint64 traceblock[kTraceBufSize];	// 8 bytes per trace entry
  uint64 ipcblock[kIpcBufSize];		// One byte per trace entry

  offset = 0;
  block_num = 0;
  size_t n;
  while ((n = fread(traceblock, 1, sizeof(traceblock), f)) != 0) {
    bool subpar_block = false; 
    subpar_block |= CheckTraceBlock(n, traceblock);	// Sets flags at first block
    offset += n;

    if (HasIPC(flags)) {
      // Extract 8KB IPC block
      n = fread(ipcblock, 1, sizeof(ipcblock), f);
      subpar_block |= CheckIpcBlock(n, ipcblock);
      offset += n;
    }
    ++total_block_count;
    if (subpar_block) {++total_bad_block_count;}

    ++block_num;
  }
  fclose(f);
  FinishBlockEvents();

  // Reset verbose and counting state
  verbose = verbose_save;
  total_msg_count = 0;
  block_num = -1;	// Further messages are not specific to any block

  // Print block summary
  if (0 < total_bad_block_count) {
    snprintf(gTempPrintBuffer, kMaxPrintBuffer, "%llu/%llu", total_bad_block_count, total_block_count);
    Note(WARN, TR_BADCOUNT, traceblock, 0*8, gTempPrintBuffer); 
  } else {
    snprintf(gTempPrintBuffer, kMaxPrintBuffer, "%llu", total_block_count);
    Note(GOOD, TR_GOODCOUNT, traceblock, 0*8, gTempPrintBuffer); 
  }

  // Check for full-trace issues
  CheckEventCounts();

  // Always do these last...
  // Print trace summary
  snprintf(gTempPrintBuffer, kMaxPrintBuffer, "%d CPUs%s%s", 
    max_cpu + 1, 
    HasIPC(flags) ? ", IPC" : "", 
    HasWrap(flags) ? ", WRAP" : "");
  Note(INFO, TR_INFO, traceblock, 0*8, gTempPrintBuffer); 
 
  // Print peaks
  if (!quiet) {
  //fprintf(stdout, "\n");
  fprintf(stdout, "     Most active 1/10 second %s has ~%lluK events (%lluK/sec/cpu)\n", 
    FormatUsecDateTime(peak_100msec * 100000), 
    peak_100msec_events >> 10,
    ((peak_100msec_events * 10) >> 10) / (max_cpu+1));
  fprintf(stdout, "     Most active second      %s        has ~%lluK events (%lluK/sec/cpu)\n", 
    FormatSecondsDateTime(peak_second), 
    peak_second_events >> 10,
    (peak_second_events >> 10) / (max_cpu+1));
#if 0
  // Don't bother. These seem fairly uninformative
  fprintf(stdout, "     Most active 10 seconds  %s        has ~%lluK events (%lluK/sec/cpu)\n", 
    FormatSecondsDateTime(peak_10second * 10),
    peak_10second_events >> 10,
    ((peak_10second_events/10) >> 10) / (max_cpu+1));
  // TODO: most active CPU
#endif
  }

  fprintf(stdout, "%s %s\n\n", trace_fail ? "FAIL" : "PASS", fname);

  return 0;
}

