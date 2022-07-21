// Little program to turn per-CPU timespans
// into fewer larger-granularity timespans
// 
// Filter from stdin to stdout
// One command-line parameter -- 
//   granularity in microseconds. zero means 1:1 passthrough
//
// compile with g++ -O2 spantospan.cc -o spantospan
//
// dick sites 2016.11.07
// dick sites 2017.08.16
//  Updated to json format in/out text
// dick sites 2017.11.18
//  add instructions per cycle IPC support
// dsites 2022.07.07 Total rewrite
//

/***
 Design notes:
 We want the granular output to contain nearly the same total amount of time per 
 timeline as the originaly.
 We want long timespans to land in nearly the same position as originally.
 We drop a lot of decoration items but keep mark_a/b/c.
 We accumulate spans by event number, releasing an output span whenever
 the total exceeds the granularity.
 Large spans land within +/- granularity of their original

 Items that total less than granularity at the end are dropped. We compensate
 by initializing each deferred span's duration to half the granularity.
 Each combined span is represented by its first-arrived item.
 ***/

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

// Short spans accumulate by summing duration
typedef map<int, OneSpan> SpanMap;

typedef struct {
  int64 next_ts_ns;
  int64 total_deferred_ns;
  SpanMap spanmap;
} CPUstate;

static const int kMaxCpus = 80;


// Globals
int64 granularity_ns;
int output_events = 0;
bool output_buffer_full[kMaxCpus];
OneSpan buffered_span[kMaxCpus];

void PrintSpan(FILE* f, const OneSpan& onespan) {
    // Name has trailing punctuation, including ],
    fprintf(f, "[%12.8f, %10.8f, %d, %d, %d, %d, %d, %d, %d, %s\n",
            onespan.start_ts_ns / 1000000000.0, 
            onespan.duration_ns / 1000000000.0,
            onespan.cpu, onespan.pid, 
            onespan.rpcid, onespan.event, 
            onespan.arg, onespan.retval, 
            onespan.ipc, onespan.name);
}

// Accumulate a span in per-CPU state, incrementing the deferred not-yet-output times
void AddSpan(const OneSpan& onespan, CPUstate* cpustate) {
  int event = onespan.event;
  SpanMap::iterator it = cpustate->spanmap.find(event);
  if (it == cpustate->spanmap.end()) {
    // Make a new event entry
    OneSpan temp;
    temp = onespan;					// Copy all the fields
    temp.duration_ns = 0;				// Updated below
    cpustate->spanmap[event] = temp;
    it = cpustate->spanmap.find(event);
//fprintf(stderr, "New "); PrintSpan(stderr, onespan);
  }
  OneSpan* addedspan = &it->second;
  if (addedspan->duration_ns == 0) {
    *addedspan = onespan;				// Reinit pid, etc.
  } else {
    addedspan->duration_ns += onespan.duration_ns;	// Just add to existing duration
  }
  cpustate->total_deferred_ns += onespan.duration_ns;
}

OneSpan* FindLargestDeferred(SpanMap& spanmap) {
  int max_deferred = 0;
  OneSpan* retval = NULL;
  for (SpanMap::iterator it = spanmap.begin(); it != spanmap.end(); ++it) {
    if (max_deferred < it->second.duration_ns) {
      max_deferred = it->second.duration_ns;
      retval = &it->second;
    }
  }
  return retval;  
}

// Run a one-span buffer so we can combine identical-event spans
// This can be called with newspan=NULL to flush the last buffered entry
void OutputSpan(int cpu, int64 next_ts_ns, const OneSpan* newspan) {
  // Possibly combine with previously buffered span per CPU
  if ((newspan != NULL) && 
      output_buffer_full[cpu] &&
      (newspan->event == buffered_span[cpu].event)) {
    buffered_span[cpu].duration_ns += newspan->duration_ns;
    return;
  }
  // Flush any buffered span
  if (output_buffer_full[cpu]) {
    PrintSpan(stdout, buffered_span[cpu]);
    ++output_events;
    output_buffer_full[cpu] = false;
  }
  // Save as new buffered span
  if (newspan != NULL) {
    buffered_span[cpu] = *newspan;			// Copy all the fields
    buffered_span[cpu].start_ts_ns = next_ts_ns;
    output_buffer_full[cpu] = true;
  }
}

void DumpDeferred(FILE* f, CPUstate* cpustate) {
  fprintf(f, "DumpDefered %5lld\n", cpustate->total_deferred_ns);
  for (SpanMap::iterator it = cpustate->spanmap.begin(); it != cpustate->spanmap.end(); ++it) {
    if (0 < it->second.duration_ns) {
      fprintf(f, "  %5lld %s\n", it->second.duration_ns, it->second.name);
    }
  }
}

OneSpan* GetCurrent(int event,  CPUstate* cpustate) {
  SpanMap::iterator it = cpustate->spanmap.find(event);
  if (it == cpustate->spanmap.end()) {
    // No such event
    return NULL;
  }
  return &it->second;
}

// Flush the deferred event that matches onespan.event
void FlushCurrent(const OneSpan& onespan, CPUstate* cpustate) {
  OneSpan* curspan = GetCurrent(onespan.event, cpustate);
  if (curspan == NULL) {return;}
  int64 duration_ns = curspan->duration_ns;
  if (duration_ns == 0) {return;}
  int cpu = curspan->cpu;
  OutputSpan(cpu, cpustate->next_ts_ns, curspan);
//fprintf(stderr, "  ->  "); PrintSpan(stderr, *curspan);
//fprintf(stderr, "\n");
  curspan->duration_ns = 0;
  cpustate->next_ts_ns += duration_ns;
  cpustate->total_deferred_ns -= duration_ns;
}

// Output deferred spans by decreasing size
void FlushDeferred(CPUstate* cpustate) {
    //DumpDeferred(stderr, &cpustate[cpu]);
    while (cpustate->total_deferred_ns >= granularity_ns) {
      OneSpan* deferspan = FindLargestDeferred(cpustate->spanmap);
      if (deferspan == NULL) {break;}
      int64 duration_ns = deferspan->duration_ns;
      OutputSpan(deferspan->cpu, cpustate->next_ts_ns, deferspan);
      //fprintf(stderr, "  =>  "); PrintSpan(stderr, *deferspan);
      deferspan->duration_ns = 0;
      cpustate->next_ts_ns += duration_ns;
      cpustate->total_deferred_ns -= duration_ns;
    }
    //fprintf(stderr, " = %lld\n", cpustate[cpu]->total_deferred_ns);
}

// Defer this span by adding its duration to accumulated time by event number,
// and also to total deferred time per CPU number.
// If total deferred for this CPU then exceeds granularity, flush the largest
// deferred spans.  
void ProcessSpan(const OneSpan& onespan, CPUstate* cpustate) {
  int cpu = onespan.cpu;
  // Initialize start timestamp at first entry per CPU
  if (cpustate[cpu].next_ts_ns < 0) {
    cpustate[cpu].next_ts_ns = onespan.start_ts_ns;
  } 
////fprintf(stderr, "%5lld in", cpustate[cpu].total_deferred_ns); PrintSpan(stderr, onespan);

  // If this is a big span,  catch up deferred spans until we are within 
  // granularity of the new span's original start_ts, and then output this span.
  // If  not big, defer this span and return.
  // Big means that this span's duration plus any same-event deferred 
  // duration is >= granularity.
//fprintf(stderr, "calling getcurrent\n");
  OneSpan* curspan = GetCurrent(onespan.event, &cpustate[cpu]);
  int64 dur_ns = onespan.duration_ns;
  if (curspan != NULL) {
    dur_ns += curspan->duration_ns;
  }
  bool bigspan = (dur_ns >= granularity_ns);
//fprintf(stderr, "bigspan %d\n", bigspan);  
  if (bigspan) {
    FlushDeferred(&cpustate[cpu]);
    AddSpan(onespan, &cpustate[cpu]);
    FlushCurrent(onespan, &cpustate[cpu]);
    return;
  }

  // Else just accumulate this span in deferred per-CPU state, possibly merging 
  // with previous small instances
  AddSpan(onespan, &cpustate[cpu]);
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

bool KeepIntact(const OneSpan& onespan) {
  // Keep mark_a for landmarks 
  if (onespan.event == 0x020A) {return true;}
  return false;
}

bool DeleteMe(const OneSpan& onespan) {
  if (onespan.cpu < 0) {return true;}
  if (onespan.event < 0x400) {return true;}
  if (onespan.duration < 0.000000011) {return true;}
  return false;
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
  CPUstate cpustate[kMaxCpus];
  // Internally, we keep everything as integer nanoseconds to avoid roundoff 
  // error and to give clean truncation
  int64 output_granularity_ns;

  if (argc < 2) {Usage();}
  granularity_ns = 1000 * atoi(argv[1]);

  // Initialize each CPU deferral
  for (int cpu = 0; cpu < kMaxCpus; ++cpu) {
    cpustate[cpu].next_ts_ns = -1;
    cpustate[cpu].total_deferred_ns = granularity_ns / 2;
    cpustate[cpu].spanmap.clear();
    output_buffer_full[cpu] = false;
  }

  // expecting:
  //    ts           dur        cpu pid  rpc event arg retval  ipc name 
  //  [ 22.39359781, 0.00000283, 0, 1910, 0, 67446, 0, 256, 3, "gnome-terminal-.1910"],

  char buffer[kMaxBufferSize];
  while (ReadLine(stdin, buffer, kMaxBufferSize)) {
//fprintf(stderr, "%s\n", buffer);
   // Zero granularity means 1:1 passthrough
    if (granularity_ns == 0) {
      fprintf(stdout, "%s\n", buffer);
      // Leading "[" below picks off just span JSON entries
      if (buffer[0] == '[') {
        ++output_events;
      }
      continue;
    }

    char buffer2[256];
    buffer2[0] = '\0';
    OneSpan onespan;
    // Leading "[" below picks off just span JSON entries
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

    // Always strip 999.0 end marker and exit this loop
    if (onespan.start_ts >= 999.0) {
      break;
    }

    // Keep a few things, such as mark_a marker
    if (KeepIntact(onespan)) {
      fprintf(stdout, "%s\n", buffer);
      ++output_events;
      continue;
    }

    // Delete all but events that span actual CPU time
    if (DeleteMe(onespan)) {
      continue;
    }

    if (kMaxCpus <= onespan.cpu){
      fprintf(stderr, "Bad CPU number at '%s'\n", buffer);
      fprintf(stdout, "Bad CPU number at '%s'\n", buffer);
      exit(0);
    }

    // Make all times nsec
    onespan.start_ts_ns = onespan.start_ts * 1000000000.0;
    onespan.duration_ns = onespan.duration * 1000000000.0;

    // Defer and then possibly output this event
    ProcessSpan(onespan, &cpustate[0]);    
  }

  // Flush any remaining deferred spans per CPU
  //fprintf(stderr, "flush all\n");
  for (int cpu = 0; cpu < kMaxCpus; ++cpu) {
    // Possibly many deferred events
    FlushDeferred(&cpustate[cpu]);
    // And push out last buffered item
    OutputSpan(cpu, cpustate[cpu].next_ts_ns, NULL);
  }

  // Add marker and closing at the end
  // Zero granularity means 1:1 passthrough
  if (granularity_ns != 0) {
    FinalJson(stdout);
  }

  fprintf(stderr, "spantospan: %d events\n", output_events);

  return 0;
}
