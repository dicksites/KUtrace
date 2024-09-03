// Little program to turn per-CPU timespans into fewer larger-granularity spans
// Copyright 2021 Richard L. Sites
// 
// Filter from stdin to stdout
// One command-line parameter -- 
//   granularity in microseconds. zero means 1:1 passthrough
//
// dick sites 2016.11.07
// dick sites 2017.08.16
//  Updated to json format in/out text
// dick sites 2017.11.18
//  add optional instructions per cycle IPC support
//

#include <map>
#include <string>

#include <stdio.h>
#include <stdlib.h>     // exit
#include <string.h>
#include "basetypes.h"

#define UserPidNum       0x200

using std::string;
using std::map;

typedef struct {
  double start_ts;	// Seconds
  double duration;	// Seconds
  int64 start_ts_ns;
  int64 duration_ns;
  int cpu;
  int pid;
  int rpcid;
  int event;
  int arg;
  int retval;
  int ipc;
  char name[64];
} OneSpan;

typedef map<int, OneSpan> SpanMap;	// Accumulated duration for each event

typedef struct {
  int64 next_ts_ns;
  int64 total_excess_ns;
  SpanMap spanmap;
} CPUstate;


int output_events = 0;

// Accumulate a span, incrementing the excess not-yet-output times
void AddSpan(const OneSpan& onespan, CPUstate* cpustate) {
  int event = onespan.event;
  SpanMap::iterator it = cpustate->spanmap.find(event);
  if (it == cpustate->spanmap.end()) {
    // Make a new entry
    OneSpan temp;
    temp = onespan;
    temp.duration_ns = 0;					// Updated below
    cpustate->spanmap[event] = temp;
    it = cpustate->spanmap.find(event);
  }
  OneSpan* addspan = &it->second;
  addspan->duration_ns += onespan.duration_ns;		// Everything else ignored
  cpustate->total_excess_ns += onespan.duration_ns;
}

OneSpan* FindLargestExcess(SpanMap& spanmap) {
  int max_excess = 0;
  OneSpan* retval = NULL;
  for (SpanMap::iterator it = spanmap.begin(); it != spanmap.end(); ++it) {
    if (max_excess < it->second.duration_ns) {
      max_excess = it->second.duration_ns;
      retval = &it->second;
    }
  }
  return retval;  
}

// Round toward zero
inline int64 RoundDown(int64 a, int64 b) {
  return (a / b) * b;
}

// Round up or down
inline int64 Round(int64 a, int64 b) {
  return ((a + (b/2)) / b) * b;
}

// Each call will update the current duration for this CPU and emit it
void ProcessSpan(int64 output_granularity_ns, 
                 const OneSpan& onespan, CPUstate* cpustate) {
  int cpu = onespan.cpu;
  if (cpustate[cpu].next_ts_ns < 0) {
    cpustate[cpu].next_ts_ns = RoundDown(onespan.start_ts_ns, output_granularity_ns);
  }
  AddSpan(onespan, &cpustate[cpu]);
  while (cpustate[cpu].total_excess_ns > output_granularity_ns) {
    OneSpan* subspan = FindLargestExcess(cpustate[cpu].spanmap);
    if (subspan == NULL) {break;}
    // Output this span, setting start time and duration to
    // multiples of output_granularity_ns.

    // If this rounds up, residual duration is negative
    int64 duration_ns = Round(subspan->duration_ns, output_granularity_ns);
    if (duration_ns <= 0) {break;}
    // Name has trailing punctuation, including ],
    fprintf(stdout, "[%12.8f, %10.8f, %d, %d, %d, %d, %d, %d, %d, %s\n",
            cpustate[cpu].next_ts_ns / 1000000000.0, duration_ns / 1000000000.0,
            subspan->cpu, subspan->pid, subspan->rpcid, subspan->event, 
            subspan->arg, subspan->retval, subspan->ipc, subspan->name);
    ++output_events;
    subspan->duration_ns -= duration_ns;
    cpustate[cpu].next_ts_ns += duration_ns;
    cpustate[cpu].total_excess_ns -= duration_ns;
  }
}

// Add dummy entry that sorts last, then close the events array and top-level json
// Version 3 with IPC
void FinalJson(FILE* f) {
  fprintf(f, "[999.0, 0.0, 0, 0, 0, 0, 0, 0, 0, \"\"]\n");	// no comma
  fprintf(f, "]}\n");
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

// Input is a json file of spans
// start time and duration for each span are in seconds
// Output is a smaller json file of fewer spans with lower-resolution times
void Usage() {
  fprintf(stderr, "Usage: spantospan resolution_usec [start_sec [stop_sec]]\n");
  exit(0);
}

//
// Filter from stdin to stdout
//
int main (int argc, const char** argv) {
  CPUstate cpustate[16];
  // Internally, we keep everything as integer nanoseconds to avoid roundoff 
  // error and to give clean truncation
  int64 output_granularity_ns = 1;

  if (argc < 2) {Usage();}
  output_granularity_ns = 1000 * atoi(argv[1]);

  // Initialize to half-full
  for (int i = 0; i < 16; ++i) {
    cpustate[i].next_ts_ns = -1;
    cpustate[i].total_excess_ns = output_granularity_ns / 2;
    cpustate[i].spanmap.clear();
  }

  // expecting:
  //    ts           dur        cpu pid  rpc event arg retval  ipc name 
  //  [ 22.39359781, 0.00000283, 0, 1910, 0, 67446, 0, 256, 3, "gnome-terminal-.1910"],

  char buffer[kMaxBufferSize];
  while (ReadLine(stdin, buffer, kMaxBufferSize)) {
    // zero granularity means 1:1 passthrough
    if (output_granularity_ns == 0) {
      fprintf(stdout, "%s\n", buffer);
      if (buffer[0] == '[') {++output_events;}
      continue;
    }

    char buffer2[256];
    buffer2[0] = '\0';
    OneSpan onespan;
    int n = sscanf(buffer, "[%lf, %lf, %d, %d, %d, %d, %d, %d, %d, %s",
                   &onespan.start_ts, &onespan.duration, 
                   &onespan.cpu, &onespan.pid, &onespan.rpcid, 
                   &onespan.event, &onespan.arg, &onespan.retval, &onespan.ipc, onespan.name);
    // fprintf(stderr, "%d: %s\n", n, buffer);
    
    if (n < 9) {
      // Copy unchanged anything not a span
      fprintf(stdout, "%s\n", buffer);
      continue;
    }

    if (onespan.cpu < 0) {continue;}

    if (onespan.start_ts >= 999.0) {break;}	// Always strip 999.0 end marker and stop

    if (16 <= onespan.cpu){
      fprintf(stderr, "Bad CPU number at '%s'\n", buffer);
      exit(0);
    }

    // If the input span is a major marker (i.e. Mark_a _b or _c) keep it now
    // And chsange no other state
    if ((0x020A <= onespan.event) && (onespan.event <= 0x020C)) {
      // Name has trailing punctuation, including ],
      fprintf(stdout, "%s\n", buffer);
      ++output_events;
      continue;
    }

    // Make all times nsec
    onespan.start_ts_ns = onespan.start_ts * 1000000000.0;
    onespan.duration_ns = onespan.duration * 1000000000.0;

    // Event is already composite
    ProcessSpan(output_granularity_ns, onespan, &cpustate[0]);    
  }

  // Add marker and closing at the end
  // zero granularity means 1:1 passthrough
  if (output_granularity_ns != 0) {
    FinalJson(stdout);
  }

  fprintf(stderr, "spantospan: %d events\n", output_events);

  return 0;
}
