// Little program to convert span rows to profile per row
// 
// Filter from stdin to stdout, producing row profile(d) or group profile JSON
//
// Copyright 2021 Richard L. Sites
//
// Compile with g++ -O2 spantoprof.cc -o spantoprof
//

#include <map>
#include <set>
#include <string>
#include <utility>	// for pair

#include <stdio.h>
#include <stdlib.h>     // exit
#include <string.h>

#include "basetypes.h"
#include "kutrace_lib.h"


using std::map;
using std::multimap;
using std::set;
using std::string;

#define pid_idle         0
#define event_idle       (0x10000 + pid_idle)

static const int SUMM_CPU = 0;
static const int SUMM_PID = 1;
static const int SUMM_RPC = 2;

static const int SortByCpuNumber = 0;
static const int SortByBasenameDotElapsed = 1;
static const int SortByBasenameUnderscoreElapsed = 2;

static const double kTEN_NSEC = 0.000000010;

static const char* kPresorted = " \"presorted\"";	// Note space

// These label the group summary rows
static const char* kSuffix[32] = {
  "_1us", "_2us", "_4us", "_8us", "_16us", "_32us", "_64us", "_125us", "_250us", "_500us", 
  "_1ms", "_2ms", "_4ms", "_8ms", "_16ms", "_32ms", "_64ms", "_125ms", "_256ms", "_512ms", 
  "_1s", "_2s", "_4s", "_8s", "_16s", "_32s", "_64s", "_128s", "_256s", "_512s", 
  "_1Ks", "_2Ks"
};

// These sort in descending order of lg(elapsed time)
static const char* kSortSuffix[32] = {
  "_31", "_30", "_29", "_28", "_27", "_26", "_25", "_24", "_23", "_22", 
  "_21", "_20", "_19", "_18", "_17", "_16", "_15", "_14", "_13", "_12", 
  "_11", "_10", "_09", "_08", "_07", "_06", "_05", "_04", "_03", "_02", 
  "_01", "_00"
};

// Map granular IPC values 0..15 to midpoint multiple of 1/16 per range
//   thus, 0..1/8 maps to 1/16 and 3.5..4 maps to 3.75 = 60/16
static const double kIpcToLinear [16] = {
  1.0, 3.0, 5.0, 7.0, 9.0, 11.0, 13.0, 15.0,  
  18.0, 22.0, 26.0, 30.0,  36.0, 44.0, 52.0, 60.0
};

// Going the other way, round down to 0..15
static const int kLinearToIpc[64] = {
  0,0, 1,1, 2,2, 3,3, 4,4, 5,5, 6,6, 7,7,
  8,8,8,8, 9,9,9,9, 10,10,10,10, 11,11,11,11,
  12,12,12,12,12,12,12,12, 13,13,13,13,13,13,13,13,
  14,14,14,14,14,14,14,14, 15,15,15,15,15,15,15,15
};  

// Each JSON input record
typedef struct {
  double start_ts;	// Seconds
  double duration;	// Seconds
  int cpu;
  int pid;
  int rpcid;
  int eventnum;
  int arg;
  int retval;
  int ipc;
  string name;
} OneSpan;

// This aggregates a number of identical events by name, summing their durations
// including a weights IPC sum
typedef struct {
  double start_ts;
  double duration;
  double ipcsum;	// Seconds * sixteenths of an IPC
  int eventnum;
  int arg;
  string event_name;
} EventTotal;

// These are all the events in a row
typedef map<string, EventTotal> RowSummary;

// We use this for sorting events in a row by start_ts 
// There will be duplicates, so multimap
typedef multimap<double, const EventTotal*> RowSummaryDP;

///// We use this for sorting events in a row by some string criterion 
//typedef multimap<string, const EventTotal*> RowSummarySP;

// This aggregates one or more identically-named rows within CPUs, PIDs, RPCs
// Each has a summary of the contained events for those rows
typedef struct {
  double lo_ts;
  double hi_ts;
  int rownum;
  int rowcount;		// The number of rows merged together here
  bool proper_row_name;
  string row_name;
  RowSummary rowsummary;
} RowTotal;

// These are all the rows in a group (Cpu, Pid, Rpc)
// indexed by cpu/pid/rpc number
typedef map<int, RowTotal> GroupSummary;

// These are all the rows in a group (Cpu, Pid, Rpc)
// indexed by a name
typedef map<string, RowTotal> GroupSummary2;

// We use this for sorting rows in a group by some string criterion 
typedef multimap<string, const RowTotal*> GroupSummarySP;


// Top-level data structure
typedef struct {
  // Profile of KUtrace spans across cpu,pid,rpc rows
  // Level 1 totals across single rows
  GroupSummary cpuprof;
  GroupSummary pidprof;
  GroupSummary rpcprof;
  // Level 2 totals across similar-name rows within a group
  GroupSummary2 cpuprof2;
  GroupSummary2 pidprof2;
  GroupSummary2 rpcprof2;
} Summary;


// Globals
static int span_count = 0;
static Summary summary;		// Aggregates across the entire trace	

static bool dorow = true;	// default to -row
static bool dogroup = false;
static bool doall = false;	// if true, show even one-row merges
static bool verbose = false;

static int output_events = 0;


void DumpSpan(FILE* f, const char* label, const OneSpan* span) {
  fprintf(f, "%s <%12.8lf %10.8lf %d  %d %d %d %d %d %d %s>\n", 
  label, span->start_ts, span->duration, span->cpu, 
  span->pid, span->rpcid, span->eventnum, span->arg, span->retval, span->ipc, span->name.c_str());
}

void DumpSpanShort(FILE* f,  const OneSpan* span) {
  fprintf(f, "<%12.8lf %10.8lf ... %s> ", span->start_ts, span->duration, span->name.c_str());
}

void DumpEvent(FILE* f, const char* label, const OneSpan& event) {
  fprintf(f, "%s [%12.8lf %10.8lf %d  %d %d %d %d %d %d %s]\n", 
  label, event.start_ts, event.duration, event.cpu, 
  event.pid, event.rpcid, event.eventnum, event.arg, event.retval, event.ipc, event.name.c_str());
}

void DumpOneEvent(FILE* f, const EventTotal& eventtotal) {
  fprintf(f, "    [%d] %12.8lf %10.8lf %10.8lf %s\n", 
          eventtotal.eventnum, eventtotal.start_ts, eventtotal.duration, eventtotal.ipcsum, eventtotal.event_name.c_str());
}

void DumpOneRow(FILE* f, const RowTotal& rowtotal) {
  fprintf(f, "  [%d] %12.8lf %10.8lf '%s'\n", 
             rowtotal.rownum, rowtotal.lo_ts, rowtotal.hi_ts, rowtotal.row_name.c_str());
  for (RowSummary::const_iterator it = rowtotal.rowsummary.begin(); 
         it != rowtotal.rowsummary.end(); 
         ++it) {
    const EventTotal& eventtotal = it->second;
    DumpOneEvent(f, eventtotal);
  }
}

void DumpRowSummary(FILE* f, const char* label, const GroupSummary& groupsummary) {
  fprintf(f, "\n%s\n--------\n", label);
  for (GroupSummary::const_iterator it = groupsummary.begin(); it != groupsummary.end(); ++it) {
    const RowTotal& rowtotal = it->second;
    DumpOneRow(f, rowtotal);
  }
}

void DumpRowSummary2(FILE* f, const char* label, const GroupSummary2& groupsummary) {
  fprintf(f, "\n%s\n--------\n", label);
  for (GroupSummary2::const_iterator it = groupsummary.begin(); it != groupsummary.end(); ++it) {
    const RowTotal& rowtotal = it->second;
    DumpOneRow(f, rowtotal);
  }
}

void DumpSummary(FILE* f, const Summary& summ) {
  fprintf(f, "\nDumpSummary\n===========\n");
  DumpRowSummary(f, "cpuprof", summ.cpuprof);
  DumpRowSummary(f, "pidprof", summ.pidprof);
  DumpRowSummary(f, "rpcprof", summ.rpcprof);
}
void DumpSummary2(FILE* f, const Summary& summ) {
  fprintf(f, "\nDumpSummary2\n===========\n");
  DumpRowSummary2(f, "cpuprof2", summ.cpuprof2);
  DumpRowSummary2(f, "pidprof2", summ.pidprof2);
  DumpRowSummary2(f, "rpcprof2", summ.rpcprof2);
}

string IntToString(int x) {
  char temp[24];
  sprintf(temp, "%d", x);
  return string(temp); 
}

string IntToString0000(int x) {
  char temp[24];
  sprintf(temp, "%04d", x);
  return string(temp); 
}

string DoubleToString(double x) {
  char temp[24];
  sprintf(temp, "%12.8lf", x);
  return string(temp); 
}

string MaybeExtend(string s, int x) {
  string maybe = "." + IntToString(x);
  if (s.find(maybe) == string::npos) {return s + maybe;}
  return s;
}


// Return floor of log base2 of x, i.e. the number of bits-1 needed to hold x
int FloorLg(uint64 x) {
  int lg = 0;
  uint64 local_x = x;
  if (local_x & 0xffffffff00000000LL) {lg += 32; local_x >>= 32;}
  if (local_x & 0xffff0000LL) {lg += 16; local_x >>= 16;}
  if (local_x & 0xff00LL) {lg += 8; local_x >>= 8;}
  if (local_x & 0xf0LL) {lg += 4; local_x >>= 4;}
  if (local_x & 0xcLL) {lg += 2; local_x >>= 2;}
  if (local_x & 0x2LL) {lg += 1; local_x >>= 1;}
  return lg;
}

// d is in seconds; we return lg of d in usec
// We multiply by 1024000 to make 1ms an exact power of 2.
// Buckets smaller than 125 usec are off by 2%,as are buckets > 512ms.
int DFloorLg(double d) {
  if (d <= 0.0) return 0;
  uint64 x = d * 1024000.0;
  int retval = FloorLg(x);
//fprintf(stderr, "  DFloorLg(%lf) = %d\n", d, retval);
  return retval;
}


// (2) RPC point event
bool IsAnRpc(const OneSpan& event) {
  return ((KUTRACE_RPCIDREQ <= event.eventnum) && (event.eventnum <= KUTRACE_RPCIDMID));
}
bool IsAnRpcnum(int eventnum) {
  return ((KUTRACE_RPCIDREQ <= eventnum) && (eventnum <= KUTRACE_RPCIDMID));
}

// (2) pc_sample point event
bool IsAPcSample(const OneSpan& event) {
  return ((event.eventnum == KUTRACE_PC_U) || (event.eventnum == KUTRACE_PC_K) || (event.eventnum == KUTRACE_PC_TEMP));
}
// (2) pc_sample event
bool IsAPcSamplenum(int eventnum) {
  return ((eventnum == KUTRACE_PC_U) || (eventnum == KUTRACE_PC_K) || (eventnum == KUTRACE_PC_TEMP));
}

// (3) Lock  event
bool IsALock(const OneSpan& event) {
  return ((event.eventnum == KUTRACE_LOCK_HELD) || (event.eventnum == KUTRACE_LOCK_TRY));
}
// (3) Lock event
bool IsALocknum(int eventnum) {
  return ((eventnum == KUTRACE_LOCK_HELD) || (eventnum == KUTRACE_LOCK_TRY));
}
bool IsALockTry(const OneSpan& event) {
  return (event.eventnum == KUTRACE_LOCK_TRY);
}
bool IsALockTrynum(int eventnum) {
  return (eventnum == KUTRACE_LOCK_TRY);
}
bool IsALockHeld(const OneSpan& event) {
  return (event.eventnum == KUTRACE_LOCK_HELD);
}
bool IsALockHeldnum(int eventnum) {
  return (eventnum == KUTRACE_LOCK_HELD);
}



// (3) Any kernel-mode execution event
bool IsKernelmode(const OneSpan& event) {
  return ((KUTRACE_TRAP <= event.eventnum) && (event.eventnum < event_idle));
}
bool IsKernelmodenum(int eventnum) {
  return ((KUTRACE_TRAP <= eventnum) && (eventnum < event_idle));
}

// (4)
bool IsAnIdle(const OneSpan& event) {
  return (event.eventnum == event_idle);
}
bool IsAnIdlenum(int eventnum) {
  return (eventnum == event_idle);
}
bool IsCExitnum(int eventnum) {
  return (eventnum == 0x20000);
}
bool IsAnIdleCstatenum(int eventnum) {
  return IsAnIdlenum(eventnum) || IsCExitnum(eventnum);
}

// (4) Any user-mode-execution event, in range 0x10000 .. 0x1ffff
// These includes the idle task
bool IsUserExec(const OneSpan& event) {
  return ((event.eventnum & 0xF0000) == 0x10000);
}
bool IsUserExecnum(int eventnum) {
  return ((eventnum & 0xF0000) == 0x10000);
}

// (4) These exclude the idle task
bool IsUserExecNonidle(const OneSpan& event) {
  return ((event.eventnum & 0xF0000) == 0x10000) && !IsAnIdle(event);
}
bool IsUserExecNonidlenum(int eventnum) {
  return ((eventnum & 0xF0000) == 0x10000) && !IsAnIdlenum(eventnum);
}


bool IsAWait(const OneSpan& event) {
  if (event.duration < 0) {return false;}
  if ((KUTRACE_WAITA <= event.eventnum) && (event.eventnum <= KUTRACE_WAITZ)) {return true;}
  return false;
}

bool IsAWaitnum(int eventnum) {
  if ((KUTRACE_WAITA <= eventnum) && (eventnum <= KUTRACE_WAITZ)) {return true;}
  return false;
}

bool IsAFreq(const OneSpan& event) {
  return (KUTRACE_PSTATE == event.eventnum); 
}
bool IsAFreqnum(int eventnum) {
  return (KUTRACE_PSTATE == eventnum); 
}

bool IsRowMarkernum(int eventnum) {
  return (KUTRACE_LEFTMARK == eventnum);
}

bool IncreasesCPUnum(int eventnum) {
  // Execution: traps, interrupts, syscalls, idle, user-mode, c-exit
  if (KUTRACE_TRAP <= eventnum) {return true;}
  // We still have names, specials, marks, PCsamps
  // Keep waits
  if (IsAWaitnum(eventnum)) {return true;}
  return false;
}

// True if this item contributes to non-zero CPU duration or we otherwise want to roll it up
// We keep PC samples so we can make a sampled profile
// We keep frequencies so we can give the average clock rate for each row
bool IsCpuContrib(const OneSpan& event) {
  if (event.duration < 0) {return false;}
  // Execution: traps, interrupts, syscalls, idle, user-mode, c-exit
  if (KUTRACE_TRAP <= event.eventnum) {return true;}
  // We still have names, specials, marks, PCsamps
  // Keep PCsamp and frequency
  if (IsAPcSample(event)) {return true;}	// PC sample overlay
  if (IsAFreq(event)) {return true;}		// Frequency overlay
  return false;
}

// True if this item contributes to non-zero CPU duration or we otherwise want to roll it up
// We ignore PID 0
// We keep PC samples so we can make a sampled profile
// We keep frequencies so we can give the average clock rate for each row
bool IsPidContrib(const OneSpan& event) {
  if (event.duration < 0) {return false;}
  if (event.pid <= 0) {return false;}
  // Execution: traps, interrupts, syscalls, idle, user-mode, c-exit
  if (KUTRACE_TRAP <= event.eventnum) {return true;}
  // We still have names, specials, marks, PCsamps
  // Keep waits, PCsamp, frequency, locks
  if (IsAWait(event)) {return true;}		// PID Waiting
  if (IsAPcSample(event)) {return true;}	// PC sample overlay
  if (IsAFreq(event)) {return true;}		// Frequency overlay
  if (IsALock(event)) {return true;}		// Lock overlay
  return false;
}

// True if this item contributes to non-zero CPU duration or we otherwise want to roll it up
// We ignore RPC 0
// We keep PC samples so we can make a sampled profile
// We keep frequencies so we can give the average clock rate for each row
bool IsRpcContrib(const OneSpan& event) {
  if (event.duration < 0) {return false;}
  if (event.rpcid <= 0) {return false;}
  // Execution: traps, interrupts, syscalls, idle, user-mode, c-exit
  if (KUTRACE_TRAP <= event.eventnum) {return true;}
  // We still have names, specials, marks, PCsamps
  // Keep waits, PCsamp, frequency, locks
  if (IsAWait(event)) {return true;}		// RPC Waiting
  if (IsAPcSample(event)) {return true;}	// PC sample overlay
  if (IsAFreq(event)) {return true;}		// Frequency overlay
  if (IsALock(event)) {return true;}		// Lock overlay
  return false;
}

// These have good PID row names
bool IsGoodPidName(const OneSpan& event) {
  // if (event.duration < 0) {return false;}
  if (KUTRACE_LEFTMARK == event.eventnum) {return true;}
  return IsUserExec(event);
}

// These have good RPC row names (method names)
bool IsGoodRpcName(const OneSpan& event) {
  // if (event.duration < 0) {return false;}
  if (event.rpcid == 0) {return false;}
  return IsAnRpc(event);
}


double dmin(double a, double b) {return (a < b) ? a : b;}
double dmax(double a, double b) {return (a > b) ? a : b;}


// Event keys are event names
void MergeEventInRow(const EventTotal& eventtotal, RowSummary* aggpereventsummary) {
  if (aggpereventsummary->find(eventtotal.event_name) == aggpereventsummary->end()) {
    // Add new event 
    (*aggpereventsummary)[eventtotal.event_name] = eventtotal;
    return;
  }

  EventTotal* es = &(*aggpereventsummary)[eventtotal.event_name];
  // The real action
  es->duration += eventtotal.duration;
  es->ipcsum += eventtotal.ipcsum; 
}

bool CheckRowname(const char* label, const string& rowname) {
  if(rowname.length() < 2) {
    fprintf(stderr, "Bad rowname_%s %s\n", label, rowname.c_str());
    return false;
  }
  return true;
}

// Merge rowtotal into groupaggregate[key], making a row as needed
void MergeOneRow(int rownum, const string& key, 
                 const string& rowname, const RowTotal& rowtotal, 
                 GroupSummary2* groupaggregate) {
  if (groupaggregate->find(key) == groupaggregate->end()) {
    // Add new row and name it
    RowTotal temp;
    temp.lo_ts = 0.0;
    temp.hi_ts = 0.0;
    temp.rownum = rownum;	// The cpu/pid/rpc# first encountered for this new row
    temp.rowcount = 0;
    temp.proper_row_name = true;
    temp.row_name.clear();
    temp.row_name = rowname;
//CheckRowname("a", temp.row_name);
    temp.rowsummary.clear();
//fprintf(stderr, "Merg lo/hi_ts[%s] = %12.8f %12.8f\n", 
//temp.row_name.c_str(), temp.lo_ts, temp.hi_ts);

    (*groupaggregate)[key]= temp;
//fprintf(stderr, "[%s] %s new aggregate row %d \n", key.c_str(), rowname.c_str(), rownum);
  }

  RowTotal* aggrowsumm = &(*groupaggregate)[key];
  ++aggrowsumm->rowcount;	// Count how many rows are merged together here

  // Merge in the individual events per row
  for (RowSummary::const_iterator it = rowtotal.rowsummary.begin(); 
         it != rowtotal.rowsummary.end(); 
         ++it) {
    const EventTotal& eventtotal = it->second;
    MergeEventInRow(eventtotal, &aggrowsumm->rowsummary);
  }
}

// Scan all the events in this row and calc their average duration over all merged rows
void DivideByRowcount(RowTotal* rowtotal) {
//fprintf(stderr, "DivideByRowcount [%d] %s rowcount=%d\n", 
//rowtotal->rownum, rowtotal->row_name.c_str(), rowtotal->rowcount);
  for (RowSummary::iterator it = rowtotal->rowsummary.begin(); 
         it != rowtotal->rowsummary.end(); 
         ++it) {
    EventTotal* eventtotal = &it->second;
    eventtotal->duration /= rowtotal->rowcount;
    eventtotal->ipcsum /= rowtotal->rowcount;
  }
}

// Strip off the .123 or _2us at the end, if any. 
// But do not match n leading period in ./run_me
string Basename(const string& name, const char* delim) {
  int delim_pos = name.rfind(delim);
  if ((delim_pos != string::npos) && (0 < delim_pos)) {
    return name.substr(0, delim_pos);
  }
  return name;
}

// Total up rows by name prefix, i.e. up to a period
void MergeGroupRows(const GroupSummary& groupsummary, GroupSummary2* groupaggregate) {
  for (GroupSummary::const_iterator it = groupsummary.begin(); it != groupsummary.end(); ++it) {
    const RowTotal* rowtotal = &it->second;
    double row_duration = rowtotal->hi_ts - rowtotal->lo_ts;
//if (row_duration < 0.0) {
//fprintf(stderr, "Bad duration_row\n");
//DumpOneRow(stderr, *rowtotal);
//}
    int lg_row_duration = DFloorLg(row_duration);	// lg of usec
    if (23 < lg_row_duration) {lg_row_duration = 23;}	// max bucket is [8 ...) seconds, 2**23

    string row_basename = Basename(rowtotal->row_name, ".");
    // If the basename is entirely digits, assume we have a CPU number. 
    // We want to average across all the CPUs, not 0_AVG, 1_AVG, ...
    int non_digit = row_basename.find_first_not_of("0123456789 ");
    bool is_cpu_number = (non_digit == string::npos);	// Only digits/blank

    // We want to accumulate incoming rows with the same row_basename.
    // We achive this by using the row name (including lg) as the key, ignoring the original
    // CPU#, PID#, RPC#
    string key_name = row_basename + kSortSuffix[lg_row_duration];
    string visible_name = row_basename + kSuffix[lg_row_duration];

//fprintf(stderr, "MergeGroupRows [%d] '%s' %s %s %8.6lf\n", 
//rowtotal->rownum, rowtotal->row_name.c_str(), key_name.c_str(), visible_name.c_str(), row_duration);

    // Level 1 row summary
    int row_basenum = rowtotal->rownum;
    MergeOneRow(row_basenum, key_name, visible_name, *rowtotal, groupaggregate);

    // Level 2 group summary
    // Offset row number from the first-order row numbers
    if (is_cpu_number) {
      MergeOneRow(row_basenum, "CPU_AVG",   "CPU_AVG", *rowtotal, groupaggregate);
    } else {
      MergeOneRow(row_basenum, row_basename + "_AVG", 
                  row_basename + "_AVG", *rowtotal, groupaggregate);
    }
  }

  // Now go back and divide all the aggregated durations by rowcount
  for (GroupSummary2::iterator it = groupaggregate->begin(); it != groupaggregate->end(); ++it) {
    RowTotal* aggrowtotal = &it->second;
    if (1 < aggrowtotal->rowcount) {
      char temp[24];
      sprintf(temp, " (%d)", aggrowtotal->rowcount);
      aggrowtotal->row_name += temp;
      DivideByRowcount(aggrowtotal);
    }
  }
}

// For CPU/PID/RPC summary rows, add together groups with the same name before any period,
// putting into power-of-two buckets by row duration lo_ts..hi_ts
// and also making one grand total (overall average per group).
void MergeRows(Summary* summ) {
//fprintf(stderr, "MergeRows\n");
  MergeGroupRows(summ->cpuprof, &summ->cpuprof2);
  MergeGroupRows(summ->pidprof, &summ->pidprof2);
  MergeGroupRows(summ->rpcprof, &summ->rpcprof2);
}

void Prune2(GroupSummary2* groupsummary) {
  // Go find all the basenames with basename_AVG rowcount greater than 1
  set<string> keepset;
  for (GroupSummary2::iterator it = groupsummary->begin(); it != groupsummary->end(); ++it) {
    RowTotal* rowtotal = &it->second;
    if ((1 < rowtotal->rowcount) &&
        (rowtotal->row_name.find("_AVG") !=  string::npos)) {
      string basename = Basename(rowtotal->row_name, "_");
      keepset.insert(basename);
    }
  }

  // Now prune everything with rowcount = 1 that is not in keepset
  for (GroupSummary2::iterator it = groupsummary->begin(); it != groupsummary->end(); ++it) {
    RowTotal* rowtotal = &it->second;
    if ((1 == rowtotal->rowcount) && 
        (keepset.find(Basename(rowtotal->row_name, "_")) == keepset.end())) {
      rowtotal->rowcount = 0;
    }
  }
}

// prune away all group rows that have rowcount=1,
// unless the basename_AVG count is more than 1
void PruneGroups(Summary* summ) {
  if (doall) {return;}
  Prune2(&summ->cpuprof2);
  Prune2(&summ->pidprof2);
  Prune2(&summ->rpcprof2);
}

// Input is a string integer; add one
void IncrString(string* s) {
  int subscr = s->length() - 1;
  while ((0 <= subscr) && ((*s)[subscr] >= '9')) {
    (*s)[subscr--] = '0';
  }
  if (0 <= subscr) {(*s)[subscr] += 1;}	// Else wrap around
}


// This first sorts the row items into user, kernel, other, idle
// and within each group descending by duration
//
// In addition to a mian CPU-execution and wait-non-execution timeline,
// there are several overlays wiht separate timelines:
//   frequency
//   PC samples
//   locks
void RewriteOneRow(RowTotal* rowtotal) {
  // Step (1) Build side multimap by sort keys
  RowSummaryDP sorted_row;
  for (RowSummary::const_iterator it = rowtotal->rowsummary.begin(); 
         it != rowtotal->rowsummary.end(); 
         ++it) {
    const EventTotal* eventtotal = &it->second;
    // We use the key value to sort events. Full traces are limited to 
    // 1000 seconds, so we offset by that
    // Negating the values gives us a descending sort instead of
    double key = 0.0;
    if (IsRowMarkernum(eventtotal->eventnum)) {
      key = -2000.0;	// Always first
    } else if (IsUserExecNonidlenum(eventtotal->eventnum)) {
      key = -1000.0 - eventtotal->duration;			// User
    } else if (IsKernelmodenum(eventtotal->eventnum)) {
      key = -1000.0 - eventtotal->duration;			// Kernel
    } else if (IsAPcSamplenum(eventtotal->eventnum)) {		// PC sample overlay
      key = -1000.0 - eventtotal->duration;
    } else if (IsALockHeldnum(eventtotal->eventnum)) {		// Lock overlay
      key = -1000.0 - eventtotal->duration;
    } else if (!IsAnIdleCstatenum(eventtotal->eventnum)) {	// Wait, freq/lock overlay
      key = 0.0 - eventtotal->duration;
    } else {
      // Idle is last
      key = 1000.0 - eventtotal->duration;			// Idle
    }
    sorted_row.insert(std::pair<double, const EventTotal*>(key, eventtotal));
if (verbose){
fprintf(stdout, "sorted_row[%12.8lf] =", key);
DumpOneEvent(stdout, *eventtotal);
}
  }

  // Step (2) Rewrite the underlying map into sorted order, by building
  //          a second map and swapping
  string temp_next = string("000000");
  RowSummary temp;
  for (RowSummaryDP::iterator it = sorted_row.begin(); it != sorted_row.end(); ++it) {
    const EventTotal* eventtotal = it->second;
    temp[temp_next] = *eventtotal;
    IncrString(&temp_next);
  }
  rowtotal->rowsummary.swap(temp);

  // Step (3) Rewrite the start times
  // Three running totals from zero: freq, pcsamp, other (e.g. cpu/wait)
  double cpu_prior_end_ts = 0.0;	// Main timeline
  double samp_prior_end_ts = 0.0;	// Overlay
  double freq_prior_end_ts = 0.0;	// Overlay
  double lock_prior_end_ts = 0.0;	// Overlay

  for (RowSummary::iterator it = rowtotal->rowsummary.begin(); 
         it != rowtotal->rowsummary.end(); 
         ++it) {
    EventTotal* eventtotal = &it->second;
    if (IsAFreqnum(eventtotal->eventnum)) {
      eventtotal->start_ts = freq_prior_end_ts;
      freq_prior_end_ts = eventtotal->start_ts + eventtotal->duration;
    } else if (IsAPcSamplenum(eventtotal->eventnum)) {
      eventtotal->start_ts = samp_prior_end_ts;
      samp_prior_end_ts = eventtotal->start_ts + eventtotal->duration;
    } else if (IsALocknum(eventtotal->eventnum)) {
      eventtotal->start_ts = lock_prior_end_ts;
      lock_prior_end_ts = eventtotal->start_ts + eventtotal->duration;
    } else {
      eventtotal->start_ts = cpu_prior_end_ts;
      cpu_prior_end_ts = eventtotal->start_ts + eventtotal->duration;
    }
  }

  // Track elapsed time just by the CPU/wait items, not PC/freq/lock
  rowtotal->lo_ts = 0.0;
  rowtotal->hi_ts = cpu_prior_end_ts;
if (verbose) {
  fprintf(stderr, "Rewrite lo/hi_ts[%s] = %12.8f %12.8f\n", 
          rowtotal->row_name.c_str(), rowtotal->lo_ts, rowtotal->hi_ts);
}
}

void RewritePerRowTimes(GroupSummary* groupsummary) {
  for (GroupSummary::iterator it = groupsummary->begin(); it != groupsummary->end(); ++it) {
    RowTotal* rowtotal = &it->second;
    RewriteOneRow(rowtotal);
  }
}

void RewritePerRowTimes2(GroupSummary2* groupsummary) {
  for (GroupSummary2::iterator it = groupsummary->begin(); it != groupsummary->end(); ++it) {
    RowTotal* rowtotal = &it->second;
    RewriteOneRow(rowtotal);
  }
}

// Input:  All items in row start at 0.0 but have non-zero durations
// Output: All items in row have consecutive start times, based on sort order
//         and row hi_ts is set to max of cpu,pid,and rpc total elapsed time
void RewriteStartTimes(Summary* summ) {
//fprintf(stderr, "RewriteStartTimes\n");
  RewritePerRowTimes(&summ->cpuprof);
  RewritePerRowTimes(&summ->pidprof);
  RewritePerRowTimes(&summ->rpcprof);
  RewritePerRowTimes2(&summ->cpuprof2);
  RewritePerRowTimes2(&summ->pidprof2);
  RewritePerRowTimes2(&summ->rpcprof2);
}

string GetKey(int sorttype, const RowTotal* rowtotal) {
  string key;
  double elapsed = rowtotal->hi_ts - rowtotal->lo_ts;
  switch (sorttype) {
  case SortByCpuNumber:
    key = IntToString0000(rowtotal->rownum);
    break;
  case SortByBasenameDotElapsed:
    key = Basename(rowtotal->row_name, ".") + DoubleToString(elapsed);
    break;
  case SortByBasenameUnderscoreElapsed:
    ////key = Basename(rowtotal->row_name, "_") + IntToString0000(DFloorLg(elapsed));
    key = Basename(rowtotal->row_name, "_") + DoubleToString(elapsed);
    break;
  }
  return key;
}

void SortRows(int sorttype, GroupSummary* groupsummary) {
  // Step (1) Build side multimap by sort keys
  GroupSummarySP sorted_group;
  for (GroupSummary::const_iterator it = groupsummary->begin(); it != groupsummary->end(); ++it) {
    const RowTotal* rowtotal = &it->second;
    string key = GetKey(sorttype, rowtotal);
    sorted_group.insert(std::pair<string, const RowTotal*>(key, rowtotal));
//fprintf(stderr, "SortRows_%d insert [%s]\n", sorttype, key.c_str());
  }

  // Step (2) Rewrite the underlying map into sorted order, by building
  //          a second map and swapping
  int temp_next = 0;
  GroupSummary temp;
  for (GroupSummarySP::const_iterator it = sorted_group.begin(); it != sorted_group.end(); ++it) {
    const RowTotal* rowtotal = it->second;
    temp[temp_next] = *rowtotal;
    ++temp_next;
  }
  groupsummary->swap(temp);
}

void SortRows2(int sorttype, GroupSummary2* groupsummary) {
  // Step (1) Build side multimap by sort keys
  GroupSummarySP sorted_group;
  for (GroupSummary2::const_iterator it = groupsummary->begin(); it != groupsummary->end(); ++it) {
    const RowTotal* rowtotal = &it->second;
    string key = GetKey(sorttype, rowtotal);
    sorted_group.insert(std::pair<string, const RowTotal*>(key, rowtotal));
//fprintf(stderr, "SortRows2_%d insert [%s]\n", sorttype, key.c_str());
  }

  // Step (2) Rewrite the underlying map into sorted order, by building
  //          a second map and swapping
  string temp_next = string("000000");
  GroupSummary2 temp;
  for (GroupSummarySP::const_iterator it = sorted_group.begin(); it != sorted_group.end(); ++it) {
    const RowTotal* rowtotal = it->second;
    temp[temp_next] = *rowtotal;
    IncrString(&temp_next);
  }
  groupsummary->swap(temp);
}

// Within each group, sort using various orderings. The downstream JSON will
// display items in the order encountered, wihtno further sorting.
void SortAllRows(Summary* summ) {
  //Level 1: Summaries across individual cpu#, pid#, rpc#
  SortRows(SortByCpuNumber, &summ->cpuprof);
  SortRows(SortByBasenameDotElapsed, &summ->pidprof);
  SortRows(SortByBasenameDotElapsed, &summ->rpcprof);
  // Level 2: Summaries across level 1 rows with like names within group
  SortRows2(SortByCpuNumber, &summ->cpuprof2);
  SortRows2(SortByBasenameUnderscoreElapsed, &summ->pidprof2);
  SortRows2(SortByBasenameUnderscoreElapsed, &summ->rpcprof2);
}


void WriteOneRowJson(FILE* f, int type, const RowTotal& rowtotal, int new_rownum) {
//fprintf(stderr, "WriteOneRowJson [%d] %s size=%d\n", rowtotal.rownum, rowtotal.row_name.c_str(), (int)//(rowtotal.rowsummary.size()));
  // Ignore merged rows that are redundant, marked by rowcount == zero
  if (rowtotal.rowcount == 0) {return;}

  int rownum = rowtotal.rownum;
  for (RowSummary::const_iterator it = rowtotal.rowsummary.begin(); 
         it != rowtotal.rowsummary.end(); 
         ++it) {
    const EventTotal* eventtotal = &it->second;
    double ts_sec = eventtotal->start_ts;
    double dur_sec = eventtotal->duration;
    int ipc = 0;
    if (0.0 <  eventtotal->duration) {
      ipc = eventtotal->ipcsum / eventtotal->duration;
      ipc = kLinearToIpc[ipc];	// Map back to granular
    }
    switch (type) {
    case SUMM_CPU:
      //                   ts dur cpu  pid rpc event  arg ret ipc  name
      fprintf(f, "[%12.8lf, %10.8lf, %d, %d, %d, %d, %d, %d, %d, \"%s\"],\n", 
          ts_sec, dur_sec,   new_rownum, -1, -1,   eventtotal->eventnum,
          eventtotal->arg, 0, ipc, eventtotal->event_name.c_str());
      break;
    case SUMM_PID:
      fprintf(f, "[%12.8lf, %10.8lf, %d, %d, %d, %d, %d, %d, %d, \"%s\"],\n", 
          ts_sec, dur_sec,   -1, new_rownum, -1,   eventtotal->eventnum,
          eventtotal->arg, 0, ipc, eventtotal->event_name.c_str());
      break;
    case SUMM_RPC:
      fprintf(f, "[%12.8lf, %10.8lf, %d, %d, %d, %d, %d, %d, %d, \"%s\"],\n", 
          ts_sec, dur_sec,   -1, -1, new_rownum,   eventtotal->eventnum,
          eventtotal->arg, 0, ipc, eventtotal->event_name.c_str());
      break;
    }
    ++output_events;
  }
}

int WritePerRowJson(FILE* f, int type, const GroupSummary& groupsummary, int new_rownum) {
  for (GroupSummary::const_iterator it = groupsummary.begin(); it != groupsummary.end(); ++it) {
    const RowTotal& rowtotal = it->second;
    WriteOneRowJson(f, type, rowtotal, new_rownum);
    ++new_rownum;
  }
  return new_rownum;
}

int WritePerRowJson2(FILE* f, int type, const GroupSummary2& groupsummary, int new_rownum) {
  for (GroupSummary2::const_iterator it = groupsummary.begin(); it != groupsummary.end(); ++it) {
    const RowTotal& rowtotal = it->second;
    // If rowcount is zero, ignore it
    if (0 < rowtotal.rowcount) {
      WriteOneRowJson(f, type, rowtotal, new_rownum);
      ++new_rownum;
    }
  }
  return new_rownum;
}

void WriteSummaryJsonRow(FILE* f, const Summary& summ) {
//fprintf(stderr, "WriteSummaryJsonRow\n");
  int new_rownum;
  //fprintf(stdout, "\"events\" : [\n");
  new_rownum = 0x10000;
  new_rownum = WritePerRowJson(f, SUMM_CPU, summ.cpuprof, new_rownum);
  new_rownum = WritePerRowJson(f, SUMM_PID, summ.pidprof, new_rownum);
  new_rownum = WritePerRowJson(f, SUMM_RPC, summ.rpcprof, new_rownum);
  fprintf(f, "[999.0, 0.0, 0, 0, 0, 0, 0, 0, 0, \"\"]\n");	// no comma
  fprintf(stdout, "]}\n");
}

void WriteSummaryJsonGroup(FILE* f, const Summary& summ) {
//fprintf(stderr, "WriteSummaryJsonGroup\n");
  int new_rownum;
  //fprintf(stdout, "\"events\" : [\n");
  new_rownum = 0x20000;
  new_rownum = WritePerRowJson2(f, SUMM_CPU, summ.cpuprof2, new_rownum);
  new_rownum = WritePerRowJson2(f, SUMM_PID, summ.pidprof2, new_rownum);
  new_rownum = WritePerRowJson2(f, SUMM_RPC, summ.rpcprof2, new_rownum);
  fprintf(f, "[999.0, 0.0, 0, 0, 0, 0, 0, 0, 0, \"\"]\n");	// no comma
  fprintf(stdout, "]}\n");
}


// Accumulate time for an item in rowsummary[name] 
// Keys are event names rather than event numbers
void AddItemInRow(int rownum, int eventnum, const OneSpan& item, RowSummary* rowsummary) {
  if (eventnum < 0) {return;}

  if (rowsummary->find(item.name) == rowsummary->end()) {
    // Add new event and name it
    EventTotal temp;
    temp.start_ts = 0.0;
    temp.duration = 0.0;
    temp.ipcsum = 0.0;
    temp.eventnum = eventnum;
    temp.arg = item.arg;
    temp.event_name.clear();
    temp.event_name = item.name;
    (*rowsummary)[item.name] = temp;
//fprintf(stdout, "  new event [%d,%d] %s\n", rownum, eventnum, item.name.c_str());
  }

  // The real action; aggregate (sum durations) by item name
  EventTotal* es = &(*rowsummary)[item.name];
  es->duration += item.duration;
  es->ipcsum += (item.duration * kIpcToLinear[item.ipc]);
}

// Add an item to groupsummary[rownum] 
// Rownum is cpu number, PID, or RPCid
void AddItem(const char* label, int rownum, int eventnum, const OneSpan& item, GroupSummary* groupsummary) {
//fprintf(stderr, "AddItem[%d,%d] %s\n", rownum, eventnum, item.name.c_str());
  if (rownum < 0) {return;}

  if (groupsummary->find(rownum) == groupsummary->end()) {
    // Add new row and name it
    // The very first item for this row might not have a proper name for the row;
    // we may add a better name later
    RowTotal temp;
    temp.lo_ts = 999.999999;
    temp.hi_ts = 0.0;
    temp.rownum = rownum;
    temp.rowcount = 1;
    temp.proper_row_name = false;
    temp.row_name.clear();
    temp.row_name = item.name;
//CheckRowname("b", temp.row_name);
    temp.rowsummary.clear();
//fprintf(stderr, "Additem lo/hi_ts[%s] = %12.8f %12.8f\n", 
//temp.row_name.c_str(), temp.lo_ts, temp.hi_ts);

    (*groupsummary)[rownum]= temp;
if (verbose) fprintf(stdout, "%s new row [%d] = %s\n", label, rownum, item.name.c_str());
//DumpSpan(stdout, "item:", &item);
  }

  RowTotal* rs = &(*groupsummary)[rownum];
  if (IncreasesCPUnum(eventnum)) {
    rs->lo_ts = dmin(rs->lo_ts, item.start_ts);
    rs->hi_ts = dmax(rs->hi_ts, item.start_ts + item.duration);
//fprintf(stderr, "Additem %d lo/hi_ts[%s] = %12.8f %12.8f\n", 
//eventnum, rs->row_name.c_str(), rs->lo_ts, rs->hi_ts);
  }
  AddItemInRow(rownum, eventnum, item, &rs->rowsummary);
}

// Add a proper name for groupsummary[rownum] 
void JustRowname(const char* label, int rownum, int eventnum, const OneSpan& item, GroupSummary* groupsummary) {
  if (rownum < 0) {return;}
  // if ((item.name == "-idle-") && (rownum != 0)) {return;}

  if (groupsummary->find(rownum) == groupsummary->end()) {
    // Add new row and name it
    RowTotal temp;
    temp.lo_ts = item.start_ts;
    temp.hi_ts = item.start_ts;

    temp.rownum = rownum;
    temp.rowcount = 1;
    temp.proper_row_name = true;
    temp.row_name.clear();
    temp.row_name = item.name;
//CheckRowname("c", temp.row_name);
    temp.rowsummary.clear();
//fprintf(stderr, "Just lo/hi_ts[%s] = %12.8f %12.8f\n", 
//temp.row_name.c_str(), temp.lo_ts, temp.hi_ts);

    (*groupsummary)[rownum] = temp;
if (verbose) fprintf(stdout, "%s JustRowname[%d] = %s\n", label, rownum, item.name.c_str());
  } else if ((*groupsummary)[rownum].proper_row_name == false) {
    (*groupsummary)[rownum].proper_row_name = true;
    (*groupsummary)[rownum].row_name = item.name;
//CheckRowname("d", item.name);

if (verbose) fprintf(stdout, "%s JustRowname [%d] = %s\n", label, rownum, item.name.c_str());
  }
}

// BUG: This previously overwrote main user execution if exact duplicate name
void InsertOneRowMarkers(RowTotal* rowtotal) {
  // Marker at front of first item in row, giving row label
  EventTotal left_marker;
  left_marker.start_ts = 0.0;
  left_marker.duration = 0.0;
  left_marker.ipcsum = 0.0;
  left_marker.eventnum = KUTRACE_LEFTMARK;
  left_marker.arg = 0;
  // Space character makes unique, avoiding overwrite
  left_marker.event_name = rowtotal->row_name + " ";
//if (!CheckRowname("f", rowtotal->row_name)) {DumpOneRow(stderr, *rowtotal);}

  (rowtotal->rowsummary)[left_marker.event_name] = left_marker;
}

void InsertPerRowMarkers(GroupSummary* groupsummary) {
  for (GroupSummary::iterator it = groupsummary->begin(); it != groupsummary->end(); ++it) {
    RowTotal* rowtotal = &it->second;
    InsertOneRowMarkers(rowtotal);
  }
}

void InsertPerRowMarkers2(GroupSummary2* groupsummary) {
  for (GroupSummary2::iterator it = groupsummary->begin(); it != groupsummary->end(); ++it) {
    RowTotal* rowtotal = &it->second;
    InsertOneRowMarkers(rowtotal);
  }
}

void InsertRowMarkers(Summary* summ) {
//fprintf(stderr, "InsertRowMarkers\n");
  InsertPerRowMarkers(&summ->cpuprof);
  InsertPerRowMarkers(&summ->pidprof);
  InsertPerRowMarkers(&summ->rpcprof);
  InsertPerRowMarkers2(&summ->cpuprof2);
  InsertPerRowMarkers2(&summ->pidprof2);
  InsertPerRowMarkers2(&summ->rpcprof2);
}

////inline int PackIpc(int num, int ipc) {return (num << 4) | ipc;}

// Rowname for a CPU is the cpu number in Ascii, added later
// Rowname for a PID is the firt user-mode execution span name
// Rowname for an RPC is the first rpcreq/resp span name
// The row name span  may well occur after the the first mention of that row,
// so we have the just-rowname logic
//
// For each item, accumulate it in per-CPU, per-PID, and per-RPC summaries
//
void SummarizeItem(const OneSpan& item, Summary* summary) {
  // Accumulate time in each group
  if (IsCpuContrib(item)) {
    AddItem("ce", item.cpu, item.eventnum, item, &summary->cpuprof);
  }

  if (IsPidContrib(item)) {
    AddItem("pe", item.pid, item.eventnum, item, &summary->pidprof);
  }

  if (IsRpcContrib(item)) {
    AddItem("re", item.rpcid, item.eventnum, item, &summary->rpcprof);
  }

  // Add any known-good row names
  if (IsGoodPidName(item)) {
    JustRowname("pe", item.pid, item.eventnum, item, &summary->pidprof);
  }

  if (IsGoodRpcName(item)) {
    JustRowname("re", item.rpcid, item.eventnum, item, &summary->rpcprof);
  }


//TODO: if wait item, ok. But if PC_U or PC_K, we want to separate by PC value, which is in the name. Sigh
}



// Close the events array, and prepare for event1 and event2
void SpliceJson(FILE* f) {
  fprintf(f, "],\n");
}

// Add dummy entry that sorts last, then close the events array and top-level json
void FinalJson_unused(FILE* f) {
  fprintf(f, "[999.0, 0.0, 0, 0, 0, 0, 0, 0, 0, \"\"]\n");	// no comma
  fprintf(f, "]}\n");
}

// Return true if the event is mark_a mark_b mark_c
inline bool is_mark_abc(uint64 event) {return (event == 0x020A) || (event == 0x020B) || (event == 0x020C);}



void RewriteRowNames(Summary* summ) {
  for (GroupSummary::iterator it = summ->cpuprof.begin(); it != summ->cpuprof.end(); ++it) {
    it->second.row_name = IntToString(it->first);	// The CPU number
//fprintf(stderr, "cpuprof number %d\n", it->first);
//CheckRowname("e", it->second.row_name);
  }
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

// Input is tail end of a line: "xyz..."],
// Output is part between quotes. Naive about backslash.
string StripQuotes(const char* s) {
  bool instring = false;
  string retval;
  for (int i = 0; i < strlen(s); ++i) {
    char c = s[i];
    if (c =='"') {instring = !instring; continue;}
    if (instring) {retval.append(1, c);}
  }
  return retval;
}

// Input is a json file of spans
// start time and duration for each span are in seconds
// Output is a smaller json file of fewer spans with lower-resolution times
void Usage() {
  fprintf(stderr, "Usage: spantoprof [-row | -group] [-all] [-v] \n");
  exit(0);
}

//
// Filter from stdin to stdout
//
int main (int argc, const char** argv) {
  if (argc < 0) {Usage();}

  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "-row") == 0) {dorow = true; dogroup = false;}
    else if (strcmp(argv[i], "-group") == 0) {dogroup = true; dorow = false;}
    else if (strcmp(argv[i], "-all") == 0) {doall = true;}
    else if (strcmp(argv[i], "-v") == 0) {verbose = true;}
    else Usage();
  }
  
  // expecting:
  //    ts           dur       cpu  pid  rpc event arg ret  ipc name--------------------> 
  //  [ 22.39359781, 0.00000283, 0, 1910, 0, 67446, 0, 256, 1,  "gnome-terminal-.1910"],

  char buffer[kMaxBufferSize];
  bool needs_presorted = true;
  bool do_copy = true;
  while (ReadLine(stdin, buffer, kMaxBufferSize)) {
    char buffer2[256];
    buffer2[0] = '\0';
    OneSpan onespan;
    char tempname[64];
    tempname[0] = '\0';
    int n = sscanf(buffer, "[%lf, %lf, %d, %d, %d, %d, %d, %d, %d, %s",
                   &onespan.start_ts, &onespan.duration, 
                   &onespan.cpu, &onespan.pid, &onespan.rpcid, 
                   &onespan.eventnum, &onespan.arg, &onespan.retval, &onespan.ipc, tempname);
    
    // If not a span, copy and go on to the next input line
    // This does all the leading JSON up to an including "events" : [
    if (do_copy && (n < 10)) {
      // Insert "presorted" JSON line in alphabetical order. 
      if (needs_presorted && (memcmp(buffer, kPresorted, 12) > 0)) {
        fprintf(stdout, "%s : 1,\n", kPresorted);
        needs_presorted = false;
      }
      fprintf(stdout, "%s\n", buffer);
      continue;
    }

    // We got past the initial JSON. Do not copy any more input lines
    do_copy = false;

if (verbose) {fprintf(stdout, "==%s\n", buffer);}

    onespan.name = StripQuotes(tempname);
    // Fixup freq to give unique names (moved back to rawtoevent now)
    if (IsAFreq(onespan) && (strchr(tempname, '_') == NULL)) {
      onespan.name = onespan.name + "_" + IntToString(onespan.arg);
    }
    // Fixup lock try to give unique names 
    if (IsALockTry(onespan)) {
      onespan.name[0] = '~';	// Distinguish try ~ from held = 
    }
    SummarizeItem(onespan, &summary);  // Build aggregates as we go
  }

  // All the input is read
  if (verbose) {
    fprintf(stderr, "Begin DumpSummary\n");
    DumpSummary(stderr, summary);
    DumpSummary2(stderr, summary);
    fprintf(stderr, "End DumpSummary\n");
  }

  RewriteRowNames(&summary);	// Must precede AggregateRows
  //DumpSummary(stderr, summary);
  //DumpSummary2(stderr, summary);

  MergeRows(&summary);		// Must precede InsertRowMarkers, WriteStartTimes
				// lo_ts and hi_ts are not filled in yet
  if (verbose) {
    DumpSummary(stderr, summary);
    DumpSummary2(stderr, summary);
  }

  InsertRowMarkers(&summary);	
  //DumpSummary(stderr, summary);
  //DumpSummary2(stderr, summary);

  RewriteStartTimes(&summary);	// Must precede WriteSummaryJson
				// Fills in lo_ts and hi_ts
  //DumpSummary(stderr, summary);
  //DumpSummary2(stderr, summary);

  SortAllRows(&summary);

  PruneGroups(&summary);
  //DumpSummary2(stderr, summary);

  if (dorow) {
    WriteSummaryJsonRow(stdout, summary);
  }
  if (dogroup) {
    WriteSummaryJsonGroup(stdout, summary);
  }
  
  fprintf(stderr, "spantoprof: %d events\n", output_events);

  return 0;
}
