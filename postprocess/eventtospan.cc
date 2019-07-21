// Little program to turn sorted Ascii event listings into timespans
//  covering 100% of the time on each CPU core
//  The main work is tracking returns and dealing with missing events
// dick sites 2016.10.18
// dick sites 2017.08.11
//  move position of event to just after duration
//  accept exported names
//  upgrade ts and duration to uint64 instead of int
//  Ignore comments starting with #
//  Expand span to include arg retval name
//  change output to be json
// dick sites 2017.08.21
//  add mwait-implied spans
// dick sites 2017.08.21
//  Attach initial pid name to first entry's CPU stack
// dick sites 2017.11.18
//  add optional instructions per cycle IPC support
// dick sites 2018.06.14
//  Take another run at merging in PID names
// dsites 2019.03.11
//  Grab PID from front of each traceblock
//  Move standalone return value into call span's return value
//  TODO: Duplicate sched span, once for old PID once for new
//  TODO: on context switch, just overwrite top of stack and current user-mode span
//  TODO: add arcs from make runnable to first subsequent execution
// dsites 2019.05.12
//  shorten AMD mwait to 13.3 usec
//  allow irq within BH
//  do not pop so soon on ctx inside sched
//  dummy push change to alter current span, back to its start

// Compile with  g++ -O2 eventtospan.cc -o eventtospan

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


/*TODO: 
  spantospan new ipc
  spantotrim new ipc
*/

#include <map>
#include <string>

#include <stdio.h>
#include <stdlib.h>     // exit
#include <string.h>
#include <time.h>
#include <unistd.h>     // getpid gethostname
#include <sys/time.h>   // gettimeofday
#include <sys/types.h>

#include "../control/basetypes.h"
#include "../control/kutrace_control_names.h"
#include "../control/kutrace_lib.h"

#define call_mask        0xc00
#define call_ret_mask    0xe00
#define ret_mask         0x200
#define type_mask        0xf00

#define dummy_trap       0x4ff
#define dummy_irq        0x5ff
#define dummy_syscall    0x9ff
#define largest_non_pid  0xfff
#define pid_idle         0
#define event_idle       (0x10000 + pid_idle)
#define event_c_exit     0x20000

#define sched_syscall    0x9ff
#define sched_sysret     0xbff

#define ipc_mask         0x0f

// Additional drawing events
#define ArcNum       	-3

static const int kMAX_CPUS = 80;

using std::string;
using std::map;


// Per-thread short stack of events to return to.
// These are saved/restored when a thread, i.e. pid, is context switched out
// and later starts running again, possibly on another CPU.
//  stack[0] is always a user-mode pid
//  stack[1] is a system call or interrupt or fault
//  stack[2] and [3] are nested interrupts/faults
//
// +---------------+
// | top 0..3      |
// +---------------+
//         
// +---------------+    +-------------------------------+
// |  event_stack  |    |  name_stack                   | 0
// +---------------+    +-------------------------------+
// |  event_stack  |    |  name_stack                   | 1
// +---------------+    +-------------------------------+
// |  event_stack  |    |  name_stack                   | 2
// +---------------+    +-------------------------------+
// |  event_stack  |    |  name_stack                   | 3
// +---------------+    +-------------------------------+
//
typedef struct {
  int top;		// Top of our small stack, always a user pid event
  int event_stack[4];		// One or more events that are stacked calls
  string name_stack[4];		// One or more events that are stacked calls
} ThreadState;
         


// Event, from rawtoevent
// +-----+-----+-----+-----+-----+-----+-----+-----+-----+-----------+
// | ts  | dur | cpu | pid | rpc |event| arg | ret | ipc |  name     |
// +-----+-----+-----+-----+-----+-----+-----+-----+-----+-----------+
//
typedef struct {
  uint64 start_ts;
  uint64 duration;
  int cpu;
  int pid;
  int rpcid;
  int event;
  int arg;
  int retval;
  int ipc;
  string name;
} OneSpan;


// Span, from start/end events
// After StartSpan
// +-----+-----+-----+-----+-----+-----+-----+-----+-----+-----------+
// | ts  | /// | cpu | pid | rpc |event| arg | ret | /// |  name     |
// +-----+-----+-----+-----+-----+-----+-----+-----+-----+-----------+
// After FinishSpan
// +-----+-----+-----+-----+-----+-----+-----+-----+-----+-----------+
// | ts  | dur | cpu | pid | rpc |event| arg | ret | ipc |  name     |
// +-----+-----+-----+-----+-----+-----+-----+-----+-----+-----------+
//
typedef struct {
  uint64 ts;
  uint64 duration;
  int cpu;
  int pid;
  int rpcid;
  int event;
  int arg;
  int retval;
  int ipc;
  string name;
} Event;


// Per-CPU state
// +---------------+
// | cpu_stack   o-|--> current thread's stack
// +---------------+
//   cur_span
// +-----+-----+-----+-----+-----+-----+-----+-----+-----+-----------+
// | ts  | dur | cpu | pid | rpc |event| arg | ret | ipc |  name     |
// +-----+-----+-----+-----+-----+-----+-----+-----+-----+-----------+
// +---------------+
// | ctx_switch_ts |
// +---------------+
// +---------------+
// | mwait_pending |
// +---------------+
//
typedef struct {
  ThreadState cpu_stack;	// Current call stack & span for each of kMAX_CPUS CPUs
  OneSpan cur_span;
  uint64 ctx_switch_ts;		// Used if /sched is missing
  int mwait_pending;		// eax value 00..FF		
} CPUState;

typedef map<int, ThreadState> PidState;	// State of each suspended task, by PID
typedef map<int, string> PidName;	// Name for each PID
typedef map<int, Event> PidWakeup;	// Previous wakeup event, by PID

// Globals
bool verbose = false;

static uint64 span_count = 0;
static int incoming_version = 0;  // Incoming version number, if any, from ## VERSION: 2
static int incoming_flags = 0;    // Incoming flags, if any, from ## FLAGS: 128
PidName pidnames;		  // Incoming PID name definitions
				  // Use these to replace empty names
PidWakeup pendingWakeup;	  // Any pending wakup event, to make arc from wakeup to running


// Stats
double total_usermode = 0.0;
double total_idle = 0.0;
double total_kernelmode = 0.0;
double total_other = 0.0;

// Events fall into four broad categories:
// (1) Text names for events
// (2) Point events
// (3) Kernel-mode execution
// (4) User-mode execution

// (1) Any name definition
bool IsNamedef(int event) {
  return (KUTRACE_VARLENLO <= event) && (event <= KUTRACE_VARLENHI);
}

// (2) Any point event
bool IsAPointEvent(const Event& event) {
  return ((KUTRACE_USERPID <= event.event) && (event.event < KUTRACE_TRAP));
}

// (3) Any kernel-mode execution event
bool IsKernelmode(const Event& event) {
  return ((KUTRACE_TRAP <= event.event) && (event.event < event_idle));
}

bool IsKernelmodeInt(int event) {
  return ((KUTRACE_TRAP <= event) && (event < event_idle));
}

// (4) Any user-mode-execution event, in range 0x10000 .. 0x1ffff
// These includes the idle task
bool IsUserExec(const Event& event) {
  return ((event.event & 0xF0000) == 0x10000);
}
bool IsUserExecInt(int event) {
  return ((event & 0xF0000) == 0x10000);
}

// Refinements ---------------------------------------------

// (2) UserPid point event
bool IsAContextSwitch(const Event& event) {
  return (event.event == KUTRACE_USERPID);
}

// (2) Make-runnable point event
bool IsAWakeup(const Event& event) {
  return (event.event == KUTRACE_RUNNABLE);
}


// (2) Mwait point event
bool IsAnMwait(const Event& event) {
  return (event.event == KUTRACE_MWAIT);
}

// (2) mark point event
bool IsAMark(const Event& event) {
  return ((KUTRACE_MARKA <= event.event) && (event.event <= KUTRACE_MARKD));
}

// (3)
bool IsACall(const Event& event) {
  if (IsUserExec(event)) return false;
  if (largest_non_pid < event.event) return false;
  if ((event.event & call_mask) == 0) return false;
  if ((event.event & ret_mask) != 0) return false;
  return true;
}

bool IsASyscall(const Event& event) {
  if ((event.event & call_ret_mask) == KUTRACE_SYSCALL64) {return true;}
  if ((event.event & call_ret_mask) == KUTRACE_SYSCALL32) {return true;}
  return false;
}

// (3)
bool IsOptimizedCall(const Event& event) {  // Must be a call already
  return (event.duration > 0);
}

// (3)
bool IsAReturn(const Event& event) {
  if (largest_non_pid < event.event) return false;
  if ((event.event & call_mask) == 0) return false;
  if ((event.event & ret_mask) == 0) return false;
  return true;
}

// (3)
bool IsSchedCallEvent(int event) {
  return (event == sched_syscall);
}
bool IsSchedCallEvent(const Event& event) {
  return (event.event == sched_syscall);
}
bool IsSchedReturnEvent(const Event& event) {
  return (event.event == sched_sysret);
}


// (4)
bool IsAnIdle(const Event& event) {
  return (event.event == event_idle);
}

bool IsAnIdleInt(int event) {
  return (event == event_idle);
}

// (4) These exclude the idle task
bool IsUserExecNonidle(const Event& event) {
  return ((event.event & 0xF0000) == 0x10000) && !IsAnIdle(event);
}
bool IsUserExecNonidleInt(int event) {
  return ((event & 0xF0000) == 0x10000) && !IsAnIdleInt(event);
}


// A user-mode-execution event is the pid number plus 64K
int PidToEvent(int pid) {return pid + 0x10000;}
int EventToPid(int event) {return event - 0x10000;}


// Example:
// [ 49.7328170, 0.0000032, 0, 0, 0, 1519, 0, 0, "local_timer_vector"],





void DumpSpan(FILE* f, const char* label, const OneSpan* span) {
  fprintf(f, "DumpSpan %s %ld %ld %d  %d %d %d %d %d %d %s\n", 
  label, span->start_ts, span->duration, span->cpu, 
  span->pid, span->rpcid, span->event, span->arg, span->retval, span->ipc, span->name.c_str());
}

void DumpSpanShort(FILE* f,  const OneSpan* span) {
  fprintf(f, "[%ld %ld %s ...] ", span->start_ts, span->duration, span->name.c_str());
}

void DumpStack(FILE* f, const char* label, const ThreadState* stack) {
  fprintf(f, "DumpStack %s [%d]\n", label, stack->top);
  for (int i = 0; i < 4; ++i) {
    fprintf(f, "  [%d] %05x %s\n",i, stack->event_stack[i], stack->name_stack[i].c_str());
  }
}

void DumpStackShort(FILE* f, const ThreadState* stack) {
  for (int i = 0; i <= stack->top; ++i) {
    fprintf(f, "%s ", stack->name_stack[i].c_str());
  }
}

void DumpEvent(FILE* f, const char* label, const Event& event) {
  fprintf(f, "DumpEvent %s %ld %ld %d  %d %d %d %d %d %d %s\n", 
  label, event.ts, event.duration, event.cpu, 
  event.pid, event.rpcid, event.event, event.arg, event.retval, event.ipc, event.name.c_str());
}


// Initially empty stack of -idle- running on this thread
void InitThreadState(ThreadState* t) {
  t->top = 0;
  for (int i = 0; i < 4; ++i) {
    t->event_stack[i] = event_idle;
    t->name_stack[i].clear();
    t->name_stack[i] = string("-idle-");
  }
}

// Initially -idle- running on this CPU
void InitSpan(OneSpan* s, int i) {
  memset(s, 0, sizeof(OneSpan));
  s->cpu = i;
  s->pid = pid_idle;
  s->event = event_idle;
  s->arg = 0;
  s->retval = 0;
  s->ipc = 0;
  s->name = "-idle-";
}


// Close off the current span
void FinishSpan(const Event& event, OneSpan* span) {
  // Prior span duration is up until new event timestamp
  span->duration = event.ts - span->start_ts;
  // CHECK NEGATIVE
  if (span->duration > 5000000000l) {	// 50 msec
    // Too big to be plausible with timer interrupts every 10 msec or less
    // Force short positive
    span->duration = 10;		// 10 nsec

    if (event.ts < span->start_ts) {
      // Force negative span to short positive
      fprintf(stderr, "BUG %ld .. %ld, duration negative, %ld0ns\n", 
              span->start_ts, event.ts, span->duration);
    } else {
      // Force big positive span to medium positive
      // except, ignore spans starting at 0
      if (span->start_ts != 0) {
        fprintf(stderr, "BUG %ld .. %ld, duration too big %ld\n", 
                span->start_ts, event.ts, span->duration);
        span->duration = 10000000;	// 10 msec
      }
    }
  }

// Need to match up return value with call
// 5330796455 0 2049 3  4052 0  1 0 0 write (801)    <=== arg0, no retval
// 5330796988 1 518 3  4052 0  3644 0 0 runnable (206)
// 5330797239 1 519 3  4052 0  1 0 0 sendipi (207)
// 5330797466 0 2561 3  4052 0  0 12 0 /write (a01)  <=== retval: write(1)=12

  // For IsOptimizedCall call/ret, entire span was already consumed
  // For unoptimized, span's return value is in the ending event
  if (IsAReturn(event)) {
    span->retval = event.retval;
  }

  // This span's ipc is in  the ending event
  span->ipc = event.ipc & ipc_mask;
}

// Open up a new span
void StartSpan(const Event& event, OneSpan* span) {
  span->start_ts = event.ts;
  span->duration = 0;
  span->cpu = event.cpu;
  span->pid = event.pid;
  span->rpcid = event.rpcid;
  span->event = event.event;
  span->arg = event.arg;
  span->retval = event.retval;
  span->ipc = 0;
  span->name = event.name;
}

void MakeArcSpan(const Event& event1, const Event& event2, OneSpan* span) {
  span->start_ts = event1.ts;
  span->duration = event2.ts - event1.ts;
  span->cpu = event1.cpu;
  span->pid = event1.pid;
  span->rpcid = event1.rpcid;
  span->event = ArcNum;
  span->arg = event2.cpu;
  span->retval = 0;
  span->ipc = 0;
  span->name = "-wakeup-";
}

// If we turned the current span idle into c_exit, now put it back
void CexitBackToIdle(OneSpan* span) {
  if (span->event != event_c_exit) {return;}
  span->event = event_idle;
  span->name = string("-idle-");
//fprintf(stdout, "CexitBackToIdle at %ld\n", span->start_ts);
}

// Make sure bugs about renaming the idle pid are gone. DEFUNCT
void CheckSpan(const char* label, const CPUState* thiscpu) {
  bool fail = false;
  const OneSpan* span = &thiscpu->cur_span;
  if ((span->name == string("-idle-")) && 
      (span->event != event_idle)) {fail = true;}
  for (int i = 0; i < 4; ++i) {
    if ((thiscpu->cpu_stack.name_stack[i] == string("-idle-")) && 
        (thiscpu->cpu_stack.event_stack[i] != event_idle)) {fail = true;}
  }
  if (fail) {
    fprintf(stderr, "\nCheckSpan failed ==================================\n");
    fprintf(stdout, "\nCheckSpan failed ==================================\n");
    DumpSpan(stdout, label, span);
    DumpStack(stdout, label, &thiscpu->cpu_stack);
  }
}

// Write the current timespan and start a new one
// Change time from multiples of 10ns to seconds
// ts           dur       CPU tid  rpc event arg0 ret  name
// Make time stamp ts 12.8 so fixed width for later text sort
void WriteSpanJson(FILE* f, const CPUState* thiscpu) {
  const OneSpan* span = &thiscpu->cur_span;
  if (span->start_ts == 0) {return;}  // Front of trace for each CPU

  // Output
  // time dur cpu pid rpcid event arg retval ipc name
  double ts_sec = span->start_ts / 100000000.0;
  double dur_sec = span->duration / 100000000.0;
  fprintf(f, "[%12.8f, %10.8f, %d, %d, %d, %d, %d, %d, %d, \"%s\"],", 
          ts_sec, dur_sec, span->cpu, 
          span->pid, span->rpcid, span->event, 
          span->arg, span->retval, span->ipc, span->name.c_str());
  ++span_count;

#if 0
  // TEMP write stack
  fprintf(f, "   %% [%d] ", thiscpu->cpu_stack.top);
  for (int i = 0; i <= thiscpu->cpu_stack.top; ++i) {
    fprintf(f, "%d %s ", thiscpu->cpu_stack.event_stack[i], thiscpu->cpu_stack.name_stack[i].c_str());
  }
#endif 
  fprintf(f, "\n");
 
  // Stastics
  if (IsUserExecNonidleInt(span->event)) {
    total_usermode += dur_sec;
  } else if (IsAnIdleInt(span->event)) {
    total_idle += dur_sec;
  } else if (IsKernelmodeInt(span->event)) {
    total_kernelmode += dur_sec;
  } else {
    total_other += dur_sec;
  }
}


// Write a point event, so they aren't lost
void WriteEventJson(FILE* f, const Event* event) {
  double ts_sec = event->ts / 100000000.0;
  double dur_sec = event->duration / 100000000.0;
  fprintf(f, "[%12.8f, %10.8f, %d, %d, %d, %d, %d, %d, %d, \"%s\"],\n", 
          ts_sec, dur_sec, event->cpu, 
          event->pid, event->rpcid, event->event,
          event->arg, event->retval, event->ipc, event->name.c_str());
  ++span_count;
}

// Open the json variable and give inital values
// Leading spaces are to keep this all in front and in order after text sort
void InitialJson(FILE* f, const char* label, const char* basetime) {
  fprintf(f, "  {\n");
  fprintf(f, " \"Comment\" : \"V2 with IPC field\",\n");
  fprintf(f, " \"axisLabelX\" : \"Time (sec)\",\n");
  fprintf(f, " \"axisLabelY\" : \"CPU Number\",\n");
  fprintf(f, " \"flags\" : %d,\n", incoming_flags);
  fprintf(f, " \"shortUnitsX\" : \"s\",\n");
  fprintf(f, " \"shortMulX\" : 1,\n");
  fprintf(f, " \"thousandsX\" : 1000,\n");
  fprintf(f, " \"title\" : \"%s\",\n", label);
  fprintf(f, " \"tracebase\" : \"%s\",\n", basetime);
  fprintf(f, " \"version\" : %d,\n", incoming_version);

  fprintf(f, "\"events\" : [\n");
}

// Add dummy entry that sorts last, then close the events array and top-level json
void FinalJson(FILE* f) {
  fprintf(f, "[999.0, 0.0, 0, 0, 0, 0, 0, 0, 0, \"\"]\n");	// no comma
  fprintf(f, "]}\n");
}

// Nesting levels are user:0, syscall:1, IRQ:2, trap:3.
// It is only legal to call to a numerically larger nesting level
// Note that we can skip levels, so these are not exactly stack depth
int NestLevel(int event) {
  if (largest_non_pid < event) {return 0;}			// User-mode pid
  if ((event & call_ret_mask) == KUTRACE_SYSCALL64) {return 1;}	// syscall/ret
  if ((event & type_mask) == KUTRACE_IRQ) {return 2;}		// interrupt
  if ((event & type_mask) == KUTRACE_TRAP) {return 3;}		// trap
  return 1;	// error; pretend it is a syscall
}

// This deals with mis-nested call
void AdjustStackForPush(const Event& event, CPUState* thiscpu) {
  while (NestLevel(event.event) <= 
         NestLevel(thiscpu->cpu_stack.event_stack[thiscpu->cpu_stack.top])) {
    // Insert dummy returns, i.e. pop, until the call is legal or we are at user-mode level
    if (thiscpu->cpu_stack.top == 0) {break;}
if (verbose) fprintf(stdout, "-%d  dummy return from %s\n", 
event.cpu, thiscpu->cpu_stack.name_stack[thiscpu->cpu_stack.top].c_str());
    --thiscpu->cpu_stack.top;
  }
}

// This deals with unbalanced return
void AdjustStackForPop(const Event& event, CPUState* thiscpu) {
  if (thiscpu->cpu_stack.top == 0) {
    // Trying to return above user mode. Push a dummy syscall
if (verbose) fprintf(stdout, "+%d dummy call to %s\n", event.cpu, event.name.c_str());
    ++thiscpu->cpu_stack.top;
    thiscpu->cpu_stack.event_stack[thiscpu->cpu_stack.top] = dummy_syscall;
    thiscpu->cpu_stack.name_stack[thiscpu->cpu_stack.top] = string("-dummy-");
  }
  // If returning from something lower nesting than top of stack,
  // pop the stack for a match. 
  int matching_call = event.event & ~ret_mask;		// Turn off the return bit
  while (NestLevel(matching_call) < 
         NestLevel(thiscpu->cpu_stack.event_stack[thiscpu->cpu_stack.top])) {
    // Insert dummy returns, i.e. pop, until the call is legal or we are at user-mode level
    if (thiscpu->cpu_stack.top == 1) {break;}
if (verbose) fprintf(stdout, "-%d  dummy return from %s\n", 
event.cpu, thiscpu->cpu_stack.name_stack[thiscpu->cpu_stack.top].c_str());
    --thiscpu->cpu_stack.top;
  }
}
/****
                                                             cpu[0]
3790782375 0 67446 0  1910 0 1910 0 gnome-terminal- (10776)  67446
3790782743 0 2567 0  1910 0 0 1 /poll (a07)                  67446 [2567]
3790783989 75 2049 0  1910 0 5 8 write (801)                 67446 2049
3790785296 46 2055 0  1910 0 60704 1 poll (807)              67446 2055
3790785360 39 2048 0  1910 0 5 8 read (800)                  67446 2048
3790793284 574 2049 0  1910 0 15 1 write (801)               67446 2049
                                                             cpu[1]
3790793888 0 71305 1  5769 0 5769 0 kworker/u4:2 (11689)     71305 
3790794567 0 50 67470 bash                                   -- 
3790794571 0 67470 1  1934 0 1934 0 bash (1078e)             67470 

3607613583 45 2049 1  0 0 1 9 write (801)
3607613711 0 2048 1  0 0 0 0 read (800)
3607613935 0 50 71446 kworker/u4:0                        
3607613941 0 71446 1  5910 0 5910 0 kworker/u4:0 (11716)     needs dummy ret

... bash running
3790870814 48 1038 1  1934 0 0 0 page_fault (40e)
3790870931 11 2062 1  1934 0 2 0 rt_sigprocmask (80e)
3790871198 23 2109 1  1934 0 65535 65526 wait4=ECHILD (83d)
3790871242 0 2063 1  1934 0 65535 0 rt_sigreturn (80f)      <== does not return
3790871406 30 1038 1  1934 0 0 0 page_fault (40e)           <== so what is running here? not -idle- bash
3790871491 64 1038 1  1934 0 0 0 page_fault (40e)
3790871699 16 1038 1  1934 0 0 0 page_fault (40e)
3790871756 27 1038 1  1934 0 0 0 page_fault (40e)
3790871861 16 2061 1  1934 0 2 0 rt_sigaction (80d)
3790871971 34 1038 1  1934 0 0 0 page_fault (40e)


[37.90871221, 0.00000021, 1, 1934, 0, 67470, 0, 6272, "bash"],
[37.90871242, 0.00000164, 1, 1934, 0, 2063, 65535, 0, "rt_sigreturn"],
[37.90871406, 0.00000030, 1, 1934, 0, 1038, 0, 0, "page_fault"],
[37.90871436, 0.00000055, 1, 1934, 0, 2063, 65535, 0, "rt_sigreturn"],
[37.90871491, 0.00000064, 1, 1934, 0, 1038, 0, 0, "page_fault"],
[37.90871555, 0.00000144, 1, 1934, 0, 2063, 65535, 0, "rt_sigreturn"],

not-sorted spans, hello_hog...
2513751730 43 2095 0  1790 0 20 65525 recvmsg=EAGAIN (82f)
2513751899 0 2055 0  1790 0 12064 0 poll (807)
2513755218 0 66507 0  971 0 971 0 dnsmasq (103cb)    <=== context switch inside poll
2513756342 0 2567 0  971 0 0 1 /poll (a07)
2513759421 0 2093 0  971 0 11 0 recvfrom (82d)

[25.13751773, 0.00000126, 0, 1790, 0, 67326, 0, 1, "compiz"],
[25.13751899, 0.00003319, 0, 1790, 0, 2055, 12064, 0, "poll"],
[25.13755218, 0.00001124, 0, 971, 0, 66507, 971, 0, "dnsmasq"],  <== context switch inside poll
[25.13756342, 0.00003079, 0, 971, 0, 66507, 0, 1, "dnsmasq"],    <=== return from prior-pid poll in new context
[25.13759421, 0.00001186, 0, 971, 0, 2093, 11, 0, "recvfrom"],
[25.13760607, 0.00002791, 0, 971, 0, 66507, 0, 49, "dnsmasq"],
[25.13763398, 0.00001808, 0, 971, 0, 2055, 41232, 0, "poll"],
[25.13732855, 0.00074986, 1, 9880, 0, 75416, 0, 65534, "WorkerPool/9880"],
[25.13807841, 0.00004114, 1, 9880, 0, 1362, 0, 0, "(552)"],
[25.13811955, 0.00007120, 1, 9880, 0, 75416, 0, 0, "WorkerPool/9880"],
[25.13765206, 0.00055158, 0, 11644, 0, 77180, 11644, 0, "bash"],
[25.13820364, 0.00000773, 0, 11644, 0, 1519, 0, 0, "local_timer_vector"],
****/


/***
Bug to be resolved...
4016863643 0 2559 0  2053 0 0 0 0 -sched- (9ff)
4016863940 0 65536 0  0 0 0 0 0 -idle- (10000)
4016864040 0 3071 0  0 0 0 0 0 /-sched- (bff)

The effect we want to get is sched: 4016863643 .. 4016864040, then idle: 4016864040 ..
i.e. move the new ctx switch target pID to just after /sched
***/

// Add the pid# to the end of user-mode name, if not already there
string AppendPid(const string& name, uint64 pid) {
  char pidnum_temp[16];
  sprintf(pidnum_temp, ".%ld", pid & 0xffff);
  if (strstr(name.c_str(), pidnum_temp) == NULL) {
    return name + string(pidnum_temp);
  }
  return name;
}

string EventNamePlusPid(const Event& event) {
  return AppendPid(event.name, event.pid); 
}

void DumpShort(FILE* f, const CPUState* thiscpu) {
  fprintf(stdout, "\t");
  DumpStackShort(stdout, &thiscpu->cpu_stack);
  fprintf(stdout, "\t");
  DumpSpanShort(stdout, &thiscpu->cur_span);
  fprintf(stdout, "\n");
}


//
// Each call will update the current duration for this CPU and emit it
//
void ProcessEvent(const Event& event, CPUState* cpustate, PidState* pidstate) {
  CPUState* thiscpu = &cpustate[event.cpu];

// Another run at the pid design 2019.03.18
// - Use user-mode-execution (IsUserExec) terminology for 0x10000 + pid events 
// - If KUTRACE_USERPID at traceblock at start of trace,
//    update stack[0]
//    update span ts/event/name via PidToEvent (if top == 0, which it is)
//    no push/pop
// - If KUTRACE_USERPID at traceblock in random middle of trace,
//    update stack[0]
//    update span ts/event/name via PidToEvent if top == 0
//    no push/pop
// - If KUTRACE_USERPID inside scheduler,
//    update stack[0]
//    update span ts/event/name via PidToEvent (if top == 0, but it isn't)
//    no push/pop
// - Have syscall to scheduler not push if in a syscall, else push (user, irq, trap)
// - Explicitly mark front of trace and check 


  // Fixup: If this event is a return from X and X is not on the stack,
  // push the corresponding call now, changing the current span to be X starting at its original ts.
  if (IsAReturn(event)) { 
    int matching_call = event.event & ~ret_mask;		// Turn off the return bit
    string matching_name = event.name.substr(1) ;		// Remove '/'
    while (NestLevel(matching_call) < 
         NestLevel(thiscpu->cpu_stack.event_stack[thiscpu->cpu_stack.top])) {
      // Insert dummy returns, i.e. pop, until the call is legal or we are at user-mode level
      if (thiscpu->cpu_stack.top == 1) {break;}
if (verbose) fprintf(stdout, "--%d  dummy return from %s\n", 
event.cpu, thiscpu->cpu_stack.name_stack[thiscpu->cpu_stack.top].c_str());
      --thiscpu->cpu_stack.top;
    }

    if (thiscpu->cpu_stack.event_stack[thiscpu->cpu_stack.top] != matching_call) {
if (verbose) fprintf(stdout, "++%d  dummy call to %s ", event.cpu, matching_name.c_str());
      ++thiscpu->cpu_stack.top;
      thiscpu->cpu_stack.event_stack[thiscpu->cpu_stack.top] = matching_call;
      thiscpu->cpu_stack.name_stack[thiscpu->cpu_stack.top] = matching_name;
      thiscpu->cur_span.event = matching_call;
      thiscpu->cur_span.name = matching_name;
      //thiscpu->cur_span.start_ts is unchanged
if (verbose) {fprintf(stdout, "Fixed "); DumpShort(stdout, thiscpu);}
    }
  }

  // Fixup: If we have a syscall/irq/fault INSIDE -sched-, pop that off,
  // but also force the current span to be the user-mode process starting at the context switch if any.
  // % [1] 2711301771 44 911 set_robust_list 	mystery3nw_opt.17230 -sched- 	[2711300782 0 -sched- ...] 
  // When we do this, be sure to put out the partial sched span
  if ((thiscpu->ctx_switch_ts > 0) && IsACall(event) && (thiscpu->cpu_stack.top == 1) && 
      (IsSchedCallEvent(thiscpu->cpu_stack.event_stack[1]))) {
if (verbose) fprintf(stdout, "==%d  call to %s but INSIDE sched", event.cpu, event.name.c_str());

    // sched span stops at ctx switch time				--------^^^^^^^^
    Event event2 = event;
    event2.ts = thiscpu->ctx_switch_ts;
    FinishSpan(event2, &thiscpu->cur_span);
    WriteSpanJson(stdout, thiscpu);	// Previous span

    --thiscpu->cpu_stack.top;
    thiscpu->cur_span.event = thiscpu->cpu_stack.event_stack[thiscpu->cpu_stack.top];
    thiscpu->cur_span.name = thiscpu->cpu_stack.name_stack[thiscpu->cpu_stack.top];
    thiscpu->cur_span.start_ts = thiscpu->ctx_switch_ts;
if (verbose) {fprintf(stdout, "Fixed "); DumpShort(stdout, thiscpu);}
  }

  // Remember that there is no pending context switch
  if (IsSchedCallEvent(event) || IsSchedReturnEvent(event)) {
    thiscpu->ctx_switch_ts = 0;
  }

  if (IsAContextSwitch(event)) {
    // Context switch
    // Current user-mode pid, seen at context switch and at front of each 
    // trace block. 
    // We expect this to match the [0] entry of the cpu's thread stack,
    // but it might not at the very front of a trace or at the oldest blocks
    // of a wraparound trace. When that happens, overwrite stack[0].
    // If the stack top is 0, also update the current span. 

    // Remember this pending context switch time, in case /sched is missing
    thiscpu->ctx_switch_ts = event.ts;

    // Turn context switch event into a user-mode-execution event at top of stack
    thiscpu->cpu_stack.event_stack[0] = PidToEvent(event.pid);
    thiscpu->cpu_stack.name_stack[0] = EventNamePlusPid(event);

    if (false && (thiscpu->cpu_stack.top == 1) && IsSchedCallEvent(thiscpu->cpu_stack.event_stack[1])) {
      // Context switch is inside scheduler. Pop out. 
      Event event2 = event;
      event2.event = sched_sysret;
      event2.name = "/-sched-";
      ProcessEvent(event2, cpustate, pidstate);	// Extra /-sched- event
      thiscpu->cpu_stack.top = 0;
    }

    // And also update the current span if we are at top
    if (thiscpu->cpu_stack.top == 0) {
      // Update user-mode-execution event at top of stack
      //VERYTEMP
      bool xx = false && (thiscpu->cur_span.event != PidToEvent(event.pid));
      if (xx) {
        fprintf(stderr, "oldevent=%05x newevent=%05x\n", thiscpu->cur_span.event, PidToEvent(event.pid));
      }
      StartSpan(event, &thiscpu->cur_span);  // userexec span start 	--------vvvvvvvv
      thiscpu->cur_span.event = thiscpu->cpu_stack.event_stack[thiscpu->cpu_stack.top]; 
      thiscpu->cur_span.name = thiscpu->cpu_stack.name_stack[thiscpu->cpu_stack.top]; 
      if (xx) {
        DumpEvent(stderr, "ctx", event);
        DumpSpan(stderr, " ctx", &thiscpu->cur_span);
        DumpStack(stderr, "ctx", &thiscpu->cpu_stack);
      }
    }
    return;
  }

// STILL A PROBLEM
// % 8efb5 5fd 0000 0000 = 585653 1533 0.0 0 0b
// 4281404362 0 1533 2  0 0  0 0 11 reschedule_ipi (5fd) <==
// % 8effa 9ff 0000 0000 = 585722 2559 0.0 0 0f
// 4281404489 0 2559 2  0 0  0 0 15 -sched- (9ff)        <== ipi exit to sched
// % 8f05e 032 0000 235e = 585822 50 0.0 9054 0d
// 4281404672 1 34 74590 bash
// -1 1 34 74590 bash
// % 8f067 200 0000 235e = 585831 512 0.0 9054 03
// 4281404688 0 512 2  9054 0  9054 0 3 bash.9054 (200)  <== still in sched but need to go to bash.9054
//                                                       <== need to pop here, force user-mode
// % 8f10d 40e 0000 0000 = 585997 1038 0.0 0 0f
// 4281404992 0 1038 2  9054 0  0 0 15 page_fault (40e)  <== but not still in sched, in bash.9054
// % 8f28b 60e 0000 0000 = 586379 1550 0.0 0 0f
// 4281405692 0 1550 2  9054 0  0 0 15 /page_fault (60e)

  // If we have a non-KUTRACE_USERPID point event, do not affect the current span. 
  // Just write the point event now, leaving the current span open to be
  // completed at a subsequent event
// 2019.05.14. Break spans at marks
  if (IsAMark(event)) {
    // Prior span stops here 					--------^^^^^^^^
    FinishSpan(event, &thiscpu->cur_span);
    WriteSpanJson(stdout, thiscpu);	// Previous span
    WriteEventJson(stdout, &event);	// Standalone mark  
  } else if (IsAPointEvent(event)) {
    WriteEventJson(stdout, &event);	// Standalone point event  
    // Remember any mwait by cpu, for drawing c-state exit sine wave
    if (IsAnMwait(event)) {thiscpu->mwait_pending = event.arg;}
    // Remember any make-runnable event by target pid, for drawing arc
    if (IsAWakeup(event)) {pendingWakeup[event.arg] = event;}
    return;
  }

  OneSpan oldspan = thiscpu->cur_span;
  // +-----+-----+-----+-----+-----+-----+-----+-----+-----+-----------+
  // | ts  | /// | cpu | pid | rpc |event| arg | ret | /// |  name     |
  // +-----+-----+-----+-----+-----+-----+-----+-----+-----+-----------+

  // Prior span stops here 					--------^^^^^^^^
  FinishSpan(event, &thiscpu->cur_span);
  WriteSpanJson(stdout, thiscpu);	// Previous span

  CexitBackToIdle(&thiscpu->cur_span);
  CexitBackToIdle(&oldspan);

  // Connect wakeup event to new span if the PID matches
  if (pendingWakeup.find(event.pid) != pendingWakeup.end()) {
    // Consume the pending wakeup
    MakeArcSpan(pendingWakeup[event.pid], event, &thiscpu->cur_span);
    WriteSpanJson(stdout, thiscpu);	// Standalone arc span
    pendingWakeup.erase(event.pid);
  }

  // Don't start new span quite yet.
  // If we have a return from foo and foo on the stack, all is good.
  // But if we have a return from foo and there is no foo on the stack, we don't
  // want to overwrite whatever is there until we push a fake foo
  
  // Optimized calls are both call/return and are treated as call
  if (IsACall(event)) {
    StartSpan(event, &thiscpu->cur_span);  // Call span start 	--------vvvvvvvv

    if (IsOptimizedCall(event)) {
      AdjustStackForPush(event, thiscpu);
      // Emit the call span now but don't push
      thiscpu->cur_span.duration = event.duration;
      // Note: Optimized call/ret, prior span ipc in ipc<3:0>, current span in ipc<7:4>
      thiscpu->cur_span.ipc = (event.ipc >> 4) & ipc_mask;
      WriteSpanJson(stdout, thiscpu);	// Standalone call-return span
      // Continue what we were doing, with new start_ts
      thiscpu->cur_span = oldspan;
      thiscpu->cur_span.start_ts = event.ts + event.duration;
    } else {
      // Non-optimized call
      // Push newly-pending call for later matching return
      AdjustStackForPush(event, thiscpu);
      ++thiscpu->cpu_stack.top;
      thiscpu->cpu_stack.event_stack[thiscpu->cpu_stack.top] = event.event;
      thiscpu->cpu_stack.name_stack[thiscpu->cpu_stack.top] = event.name;
    }

  } else if (IsAReturn(event)) {    
    // Adjust first, then startspan at proper nesting level
    AdjustStackForPop(event, thiscpu);
    --thiscpu->cpu_stack.top;
    StartSpan(event, &thiscpu->cur_span);  // Post-return span 	--------vvvvvvvv
    thiscpu->cur_span.event = thiscpu->cpu_stack.event_stack[thiscpu->cpu_stack.top]; 
    thiscpu->cur_span.name = thiscpu->cpu_stack.name_stack[thiscpu->cpu_stack.top]; 

  } else if (IsUserExec(event)) {
    int oldpid = oldspan.pid;
    int newpid = event.pid;

    // Swap out the old thread's stack
    (*pidstate)[oldpid] = thiscpu->cpu_stack;
    if (pidstate->find(newpid) == pidstate->end()) {
      // Switching to a thread we haven't seen before. Should only happen at trace start.
      // Create a one-item stack of just user-mode pid
      ThreadState temp;
      InitThreadState(&temp);
      temp.top = 0;
      temp.event_stack[0] = PidToEvent(newpid);
      temp.name_stack[0] = event.name;
      (*pidstate)[newpid] = temp;
    }
    // Swap in the new thread's stack
    thiscpu->cpu_stack = (*pidstate)[newpid];
    StartSpan(event, &thiscpu->cur_span);  // Post-switch span 	--------vvvvvvvv

// 4258610968 0 2028 3  0 0  0 0 0 /local_timer_vector (7ec)
// 4258610995 0 2559 3  0 0  0 0 0 -sched- (9ff)
// 4258611026 1 34 66628 mysqld
// -1 1 34 66628 mysqld
// 4258611031 0 66628 3  1092 0  1092 0 0 mysqld.1092 (10444)
// 4258611094 0 3071 3  1092 0  0 0 0 /-sched- (bff)
// 4258611143 0 2768 3  1092 0  0 0 0 /io_getevents (ad0)
// 4258611253 0 2256 3  1092 0  61440 0 9 io_getevents (8d0)

// [ 42.58610968, 0.00000027, 3, 0, 0, 65536, 0, 0, 0, "-idle-"],
// [ 42.58610995, 0.00000036, 3, 0, 0, 2559, 0, 0, 0, "-sched-"],
// [ 42.58611031, 0.00000063, 3, 1092, 0, 66628, 1092, 0, 0, "mysqld.1092"],
// [ 42.58611094, 0.00000049, 3, 1092, 0, 66628, 0, 0, 0, "mysqld.1092"],     BOGUS
// [ 42.58611143, 0.00000110, 3, 1092, 0, 66628, 0, 0, 9, "mysqld.1092"],     BOGUS
// [ 42.58611253, 0.00000059, 3, 1092, 0, 2256, 61440, 0, 0, "io_getevents"],

// and I lost all the c-exits


// ...   thiscpu->cur_span.event = thiscpu->cpu_stack.event_stack[thiscpu->cpu_stack.top];
//    thiscpu->cur_span.name = thiscpu->cpu_stack.name_stack[thiscpu->cpu_stack.top];

  } else {
    // c-exit and other synthesized items
    // Make it a standalone span and go back to what was running
    WriteEventJson(stdout, &event);  
    // Continue what we were doing, with new start_ts
    StartSpan(event, &thiscpu->cur_span);  // New start 	--------vvvvvvvv
    thiscpu->cur_span = oldspan;
    thiscpu->cur_span.start_ts = event.ts + event.duration;
  }
}

// ./drivers/idle/intel_idle.c
//   "C1-HSW",  0x00, .exit_latency = 2,        // 100ns ?
//   "C1E-HSW", 0x01, .exit_latency = 10,
//   "C3-HSW",  0x10, .exit_latency = 33,
//   "C6-HSW",  0x20, .exit_latency = 133,
//   "C7s-HSW", 0x32, .exit_latency = 166,
//   "C8-HSW",  0x40, .exit_latency = 300,
//   "C9-HSW",  0x50, .exit_latency = 600,
//   "C10-HSW", 0x60, .exit_latency = 2500,

  // Table entries are unspecified units; assume for the moment multiples of 100ns
static const int kLatencyTable[256] = {
    2, 10,  2,  2,   2,  2,  2,  2,   2,  2,  2,  2,   2,  2,  2,  2, 
   33, 33, 33, 33,  33, 33, 33, 33,  33, 33, 33, 33,  33, 33, 33, 33, 
  133,133,133,133, 133,133,133,133, 133,133,133,133, 133,133,133,133, 
  166,166,166,166, 166,166,166,166, 166,166,166,166, 166,166,166,166, 

  300,300,300,300, 300,300,300,300, 300,300,300,300, 300,300,300,300, 
  600,600,600,600, 600,600,600,600, 600,600,600,600, 600,600,600,600, 
  2500,2500,2500,2500, 2500,2500,2500,2500, 2500,2500,2500,2500, 2500,2500,2500,2500, 
  2500,2500,2500,2500, 2500,2500,2500,2500, 2500,2500,2500,2500, 2500,2500,2500,2500, 

  2500,2500,2500,2500, 2500,2500,2500,2500, 2500,2500,2500,2500, 2500,2500,2500,2500, 
  2500,2500,2500,2500, 2500,2500,2500,2500, 2500,2500,2500,2500, 2500,2500,2500,2500, 
  2500,2500,2500,2500, 2500,2500,2500,2500, 2500,2500,2500,2500, 2500,2500,2500,2500, 
  2500,2500,2500,2500, 2500,2500,2500,2500, 2500,2500,2500,2500, 2500,2500,2500,2500, 

  2500,2500,2500,2500, 2500,2500,2500,2500, 2500,2500,2500,2500, 2500,2500,2500,2500, 
  2500,2500,2500,2500, 2500,2500,2500,2500, 2500,2500,2500,2500, 2500,2500,2500,2500, 
  2500,2500,2500,2500, 2500,2500,2500,2500, 2500,2500,2500,2500, 2500,2500,2500,2500, 
  2500,2500,2500,2500, 2500,2500,2500,2500, 2500,2500,2500,2500, 2500,2500,2500, 133, // AMD mwait guess
};

// Mwait implies extra exit-latency to come out of power-saving C-state
// We are at the following idle.
// Turn this idle span into a shorter one followed by a C_exit span
// When called, we are about to close off the preceding -idle- very short to reflect the mwait start,
// then end up with a pending idle... [MORE]

// Change
//   4124420783 18 2055 1  1931 0 35040 1 poll (807)
//   4124420829 114 2095 1  1931 0 13 68 recvmsg (82f)
//   4124421016 0 65536 0  0 0 0 0 -idle-.0 (10000)
//   4124421199 1 520 0  0 0 32 0 Mwait (208)           <== table gives 133
//   4124421391 16 2049 1  1931 0 10 8 write (801)
// to
//   4124420783 18 2055 1  1931 0 35040 1 poll (807)
//   4124420829 114 2095 1  1931 0 13 68 recvmsg (82f)
//   4124421016 0 65536 0  0 0 0 0 -idle-.0 (10000)
//   4124421199 1 520 0  0 0 32 0 Mwait (208)
//   4124421258 133 131072 0  0 0 0 0 -C-exit- (event_c_exit)  <== ADDED
//   4124421391 16 2049 1  1931 0 10 8 write (801)

void ProcessMwait(const Event& event, CPUState* cpustate, PidState* pidstate) {
  CPUState* thiscpu = &cpustate[event.cpu];
  // Table entries are unknown units; assume for the moment multiples of 100ns
  uint64 exit_latency = kLatencyTable[thiscpu->mwait_pending] * 10;
  uint64 pending_span_latency = event.ts - thiscpu->cur_span.start_ts;
  bool good_mwait = (thiscpu->cpu_stack.top == 0); 	// Expecting to be in user-mode
  cpustate[event.cpu].mwait_pending = 0;

  if (!good_mwait) {
    // No change -- not immediately after a switch to idle 
    fprintf(stderr, "ProcessMwait ignored %ld %ld %ld %d %05x\n", 
            event.ts, exit_latency, pending_span_latency,
            thiscpu->cpu_stack.top, thiscpu->cpu_stack.event_stack[0]);
    return;
  }

  if (pending_span_latency <= exit_latency) {
    // Actual remaining idle is shorter than supposed exit latency.
    // Assume that the hardware just shortened in that case
    //fprintf(stderr, "ProcessMwait shortened %ld %ld %ld\n", 
    //       event.ts, exit_latency, pending_span_latency); 
    exit_latency = pending_span_latency;  
  } 

  Event event2 = event;
  // Insert extra c-exit event
  // event2.cpu, event2.pid, event2.rpcid unchanged
  event2.ts = event.ts - exit_latency;
  event2.duration = exit_latency;
  event2.event = event_c_exit;
  event2.arg = 0;
  event2.retval = 0;
  event2.ipc = 0;
  event2.name = "-c-exit-";
  ProcessEvent(event2, cpustate, pidstate);	// Extra c-exit event
  // But we want the pending user-mode item to remain -idle- upon exit return
  // from the interupt or whatever happened to get us out of idle
  thiscpu->cpu_stack.event_stack[0] = event_idle;
  thiscpu->cpu_stack.name_stack[0] = string("-idle-");
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

// We assign every nanosecond of each CPUs time to some time span.
// Initially, all CPUs are assumed to be executing the idle job, pid=0
// Any syscall/irq/trap pushes into that kernel code
// Any matching return pops back to the current user code
// Items can nest only in this order:
//  user ==> syscall ==> irq ==> trap
// In general, there will be missing and sometimes wrong information, so 
// this program needs to be robust in assigning time in meaningful ways.
// 
// We run a small stack for each CPU, and swap it away when there is a 
// context switch, bringing it back when there is a context switch back.
// 
// If we encounter a not-allowed transiiton, we insert pops and pushes as needed
// to make a correctly-nested set of time spans.

//
// Usage: eventtospan <event file name> [-v]
//
int main (int argc, const char** argv) {
  CPUState cpustate[kMAX_CPUS];	
  PidState pidstate;

  Event event;
  string trace_label;
  string trace_timeofday;

  if (argc >= 2) {
    // Pick off trace label from first argument, if any
    trace_label = string(argv[1]);
  }

  // Pick off other flags
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "-v") == 0) {verbose = true;}
  } 

  for (int i = 0; i < kMAX_CPUS; ++i) {
    InitThreadState(&cpustate[i].cpu_stack);
    InitSpan(&cpustate[i].cur_span, i);
    cpustate[i].ctx_switch_ts = 0;
    cpustate[i].mwait_pending = 0;
  }

  ////const char* label = "hello_world.c, partial kernel-user trace";
  ////const char* basetime = "2016-11-19_10:04:00";
  ////InitialJson(stdout, label, basetime);

  uint64 prior_ts = 0;
  int linenum = 0;
  char buffer[kMaxBufferSize];
  while (ReadLine(stdin, buffer, kMaxBufferSize)) {
    ++linenum;
    int len = strlen(buffer);
//fprintf(stdout, "%%%s\n", buffer);
    if (buffer[0] == '\0') {continue;}
    // Comments start with #
    if (buffer[0] == '#') {
      // Pull timestamp out of early comments
      // Look for first 
      // # [1] 2017-08-21_09:51:48.620665
      // Must be there. This triggers initial json output
      if ((len >= 32) && 
          trace_timeofday.empty() && 
          (memcmp(buffer, "# [1] 20", 8) == 0)) {
          // From # [1] 2019-03-16_16:43:42.571604
          // extract    2019-03-16_16:43:00
          // since the timestamps are all relative to a minute boundary
          trace_timeofday = string(buffer, 6, 17) + "00";
          fprintf(stderr, "eventtospan: trace_timeofday '%s'\n", trace_timeofday.c_str());
          InitialJson(stdout, trace_label.c_str(), trace_timeofday.c_str());
      }
      // Pull version and flags out if present
      if (memcmp(buffer, "# ## VERSION: ", 14) == 0) {incoming_version = atoi(buffer + 14); fprintf(stderr, "VERSION %d\n", incoming_version);}
      if (memcmp(buffer, "# ## FLAGS: ", 12) == 0) {incoming_flags = atoi(buffer + 12); fprintf(stderr, "FLAGS %d\n", incoming_flags);}
      continue;
    }

    // Input created by:
    //  fprintf(stdout, "%ld %ld %ld %ld  %ld %ld %ld %ld %s (%lx)\n", 
    //          mhz, duration, event, current_cpu, current_pid[current_cpu], current_rpc[current_cpu], 
    //          arg, retval, name.c_str(), event);
    // or if a name by
    //    fprintf(stdout, "%ld %ld %ld %ld %s\n", 
    //            mhz, duration, event, nameinsert, tempstring);
    // or if version 2, IPC is added before name
    //  fprintf(stdout, "%ld %ld %ld %ld  %ld %ld %ld %ld %d %s (%lx)\n", 
    //          mhz, duration, event, current_cpu, current_pid[current_cpu], current_rpc[current_cpu], 
    //          arg, retval, ipc, name.c_str(), event);
    //

    char buffer2[256];
    // Pick off the event to see if it is a name definition line
    // (This could be done with less repeated effort)
    uint64 temp_ts;
    uint64 temp_dur;
    int temp_event = 0;
    int temp_arg = 0;
    char temp_name[64];
    sscanf(buffer, "%ld %ld %d %d %s", &temp_ts, &temp_dur, &temp_event, &temp_arg, temp_name);
    if (IsNamedef(temp_event)) {
      if (IsUserExecNonidleInt(temp_arg)) {	// Just pick off PID names
//fprintf(stderr, "pidname[%d] = %s\n", temp_arg, temp_name);
        pidnames[temp_arg] = string(temp_name);
      }
      continue;
    } 

    if (incoming_version < 2) {
      event.ipc = 0;
      int n = sscanf(buffer, "%ld %ld %d %d %d %d %d %d %s",
                     &event.ts, &event.duration, &event.event, &event.cpu, 
                     &event.pid, &event.rpcid, &event.arg, &event.retval, buffer2);
      if (n != 9) {continue;}
    } else {
      int n = sscanf(buffer, "%ld %ld %d %d %d %d %d %d %d %s",
                     &event.ts, &event.duration, &event.event, &event.cpu, 
                     &event.pid, &event.rpcid, &event.arg, &event.retval,  
                     &event.ipc, buffer2);
      if (n != 10) {continue;}
    }

    // Input must be sorted by timestamp
    if (event.ts < prior_ts) {
      fprintf(stderr, "Out of time order at line[%d] %s\n", linenum, buffer);
      exit(0);
    }
    event.name = string(buffer2);

if (verbose) {
fprintf(stdout, "%% [%d] %ld %ld %03x %s ", event.cpu, event.ts, event.duration, event.event, event.name.c_str());
DumpShort(stdout, &cpustate[event.cpu]);
}

    if (kMAX_CPUS <= event.cpu){
      fprintf(stderr, "Bad CPU number at '%s'\n", buffer);
      exit(0);
    }

    // Fixup PID names
    // A PID name can be recorded in trace block 2 for CPU A at an earlier time than
    // it is mentioned in trace block 1 for CPU B. Since rawtoevent process trace 
    // blocks in order, it will see the use in block 1 before it sees the name in 
    // block 2. This leaves an empty name for the block 1 events.
    //
    // The rawtoevent output is time sorted before we run event to span, so our input
    // here will see the name first. We detect empty names here and put in longer
    // ones when available. 

    if ((IsUserExecNonidle(event) || IsAContextSwitch(event)) && 
        (buffer2[0] == '.')) {
      char maybe_better_name[64];
      int userexec_event = event.event;
      if (IsAContextSwitch(event)) {userexec_event = PidToEvent(event.pid);} 
      sprintf(maybe_better_name, "%s.%d", pidnames[userexec_event].c_str(), event.pid);
      if (strlen(maybe_better_name) > strlen(buffer2)) {
        // Do the replacement
        //fprintf(stdout, "PID %d %s => %s\n", event.event, buffer2, maybe_better_name);
        event.name = string(maybe_better_name);
      }
    }

    prior_ts = event.ts;
    
    // Turn an idle event with an mwait pending on this CPU into two
    // events -- shorter idle followed by power C-state exit latency
    if (cpustate[event.cpu].mwait_pending > 0) {
      ProcessMwait(event, &cpustate[0], &pidstate);
    }
    ProcessEvent(event, &cpustate[0], &pidstate);    
  }

  // Maybe flush the last span here
  FinalJson(stdout);

  // Statistics
  double total_dur = total_usermode + total_idle + total_kernelmode + total_other;
  total_dur *= 0.01;	// To give percents
  fprintf(stderr, 
          "eventtospan: %ld spans, %3.1f%% user, %3.1f%% sys, %3.1f%% idle, %3.1f%% other\n",
          span_count, 
          total_usermode / total_dur, total_kernelmode / total_dur, 
          total_idle / total_dur, total_other / total_dur); 
  return 0;
}
