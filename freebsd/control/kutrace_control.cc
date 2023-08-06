// Little program to control dclab_tracing 
// dick sites 2017.11.16
//
// 2017.11.16 dsites Updated to include instructions per cycle IPC flag
// 2018.05.08 dsites Updated by switching to using kutrace_lib
// 2019.02.19 dsites Updated ...
//
// This program reads commands from stdin
//
// Compile with gcc -O2 kutrace_control.cc kutrace_lib.cc -o kutrace_control

/*
 * Copyright (C) 2019 Richard L. Sites
 *
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <time.h>	// nanosleep
#include <unistd.h>     // getpid gethostname
//#include <x86intrin.h>

#include <sys/time.h>   // gettimeofday
#include <sys/types.h>

#include "basetypes.h"
#include "kutrace_control_names.h"
#include "kutrace_lib.h"

/*
TODO: 
Rationalize the use of side-effect-free DoTest
*/

/* Outgoing arg to DoReset  */
#define DO_IPC 1
#define DO_WRAP 2

////typedef long unsigned int u64;
////typedef long signed int   s64;

/* For the flags byte in traceblock[1] */
#define IPC_Flag 0x80ul
#define WRAP_Flag 0x40ul
#define Unused2_Flag 0x20ul
#define Unused1_Flag 0x10ul
#define VERSION_MASK 0x0Ful

// Module must be at least this version number for us to run
static const u64 kMinModuleVersionNumber = 3;

// Number of u64 values per trace block (64KB total)
static const int kTraceBufSize = 8192;

// Number of u64 values per IPC block, one u8 per u64 in trace buf (8KB total)
static const int kIpcBufSize = kTraceBufSize >> 3;


#if 0
// Globals for mapping cycles to gettimeofday
static int64 start_cycles = 0;
static int64 stop_cycles = 0;
static int64 start_usec = 0;
static int64 stop_usec = 0;
#endif


// Very first block layout (x86 cycle counter is rdtsc >> 6)
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
//   |           (freq)              |            PID                | 8  module
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
// All other blocks layout
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



void Usage() {
  fprintf(stderr, "usage: kutrace_control, with sysin lines\n");
  fprintf(stderr, "  init, on, off, flush, reset, stat, dump, quit\n");
  exit(0);
}

// Sleep for n milliseconds
void msleep(int msec) {
  struct timespec ts;
  ts.tv_sec = msec / 1000;
  ts.tv_nsec = (msec % 1000) * 1000000;
  nanosleep(&ts, NULL);
}


static const int kMaxBufferSize = 256;

// Read next line, stripping any crlf. Return false if no more.
bool ReadLine(FILE* f, char* buffer, int maxsize) {
  char* s = fgets(buffer, maxsize, f);
  if (s == NULL) {return false;}
  int len = strlen(s);
  // Strip any crlf or cr or lf
  if (s[len - 1] == '\n') {s[--len] = '\0';}
  if (s[len - 1] == '\r') {s[--len] = '\0';}
  return true;
}

// Take a series of commands from stdin
//
//  init	Initialize trace buffer with syscall/irq/trap names
//  on	Turn on tracing
//  off	Turn off tracing
//  flush	With tracing off, zero out the rest of each partly-used traceblock
//  reset	Set up for a new tracing run
//  stat	Show some sort of tracing status
//  dump	Dump the trace buffer to constructed filename
//  quit	Exit this program
//
// Command-line argument -force ignores any other running tracing and turns it off
//
int main (int argc, const char** argv) {
//VERYTEMP
fprintf(stderr, "Entering kutrace_control\n");

  if ((argc > 1) && (strcmp(argv[1], "-force") == 0)) {
    kutrace::DoControl(KUTRACE_CMD_RESET, 0);
  } else {
    if (!kutrace::TestModule()) {
      return 0;
    }
  }

  u64 control_flags = 0;
  // Added: if argv[1] is 1, do "go" and exit with tracing on
  //        if argv[1] is 0, do "stop" and exit with tracing off

  char fname[256];
  kutrace::MakeTraceFileName("ku", fname);

  if (argc > 1) {
    if (strcmp(argv[1], "1") == 0) {
      kutrace::DoReset(control_flags); kutrace::DoInit(argv[0]); kutrace::DoOn();
      return 0;
    }
    if (strcmp(argv[1], "0") == 0) {
      /* After DoOff wait 20 msec for any pending tracing to finish */
      kutrace::DoOff(); msleep(20); kutrace::DoFlush(); kutrace::DoDump(fname); kutrace::DoQuit();
      return 0;
    }
  }


  // Avoid always reseting so we can possibly start this program with tracing on
  char buffer[kMaxBufferSize];
  fprintf(stdout, "control> ");
  fflush(stdout);
  while (ReadLine(stdin, buffer, kMaxBufferSize)) {
    if (buffer[0] == '\0') {kutrace::DoStat(control_flags);}
    else if (strcmp(buffer, "init") == 0) {kutrace::DoInit(argv[0]);}
    else if (strcmp(buffer, "test") == 0) {kutrace::DoTest();}
    else if (strcmp(buffer, "on") == 0) {kutrace::DoOn();}
    else if (strcmp(buffer, "off") == 0) {kutrace::DoOff(); msleep(20);}
    else if (strcmp(buffer, "flush") == 0) {kutrace::DoFlush();}
    else if (strcmp(buffer, "reset") == 0) {kutrace::DoReset(control_flags);}
    else if (strcmp(buffer, "stat") == 0) {kutrace::DoStat(control_flags);}
    else if (strcmp(buffer, "dump") == 0) {kutrace::DoDump(fname);}
    else if (strcmp(buffer, "go") == 0) {
      control_flags = 0; kutrace::DoReset(control_flags); kutrace::DoInit(argv[0]); kutrace::DoOn();
    } else if (strcmp(buffer, "goipc") == 0) {
      control_flags |= DO_IPC; kutrace::DoReset(control_flags); kutrace::DoInit(argv[0]); kutrace::DoOn();
    } else if (strcmp(buffer, "gowrap") == 0) {
      control_flags |= DO_WRAP; kutrace::DoReset(control_flags); kutrace::DoInit(argv[0]); kutrace::DoOn();
    } else if ((strcmp(buffer, "goipcwrap") == 0) || (strcmp(buffer, "gowrapipc") == 0)) {
      control_flags |= (DO_IPC | DO_WRAP); kutrace::DoReset(control_flags); kutrace::DoInit(argv[0]); kutrace::DoOn();
    } else if (strcmp(buffer, "stop") == 0) {
      /* After DoOff wait 20 msec for any pending tracing to finish */
      kutrace::DoOff(); msleep(20); kutrace::DoFlush(); kutrace::DoDump(fname); control_flags = 0; kutrace::DoQuit();
    } else if (strcmp(buffer, "quit") == 0) {kutrace::DoQuit();}
    else if (strcmp(buffer, "exit") == 0) {kutrace::DoQuit();}
    else {
      fprintf(stdout, "Not recognized '%s'\n", buffer);
      fprintf(stdout, "  go goipc stop init on off flush reset stat dump quit\n");
    }

    fprintf(stdout, "control> ");
    fflush(stdout);
  }

  return 0;
}



