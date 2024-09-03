// Little program to filter time range in per-CPU timespans
// Copyright 2021 Richard L. Sites
// 
// Filter from stdin to stdout
// One or two command-line parameters -- 
//   stat_second [stop_second]
//
// dick sites 2016.11.07
// dick sites 2017.08.16
//  Cloned from json format spantospan
// dick sites 2017.09.01
//  Add trim by mark_abc label
// dick sites 2017.11.18
//  add optional instructions per cycle IPC support
//
//
// Compile with g++ -O2 spantotrim.cc from_base40.cc -o spantotrim
//

#include <map>
#include <string>

#include <stdio.h>
#include <stdlib.h>     // exit
#include <string.h>
#include "basetypes.h"
#include "from_base40.h"

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

static int incoming_version = 0;  // Incoming version number, if any, from ## VERSION: 2
static int incoming_flags = 0;    // Incoming flags, if any, from ## FLAGS: 128

// Add dummy entry that sorts last, then close the events array and top-level json
void FinalJson(FILE* f) {
  fprintf(f, "[999.0, 0.0, 0, 0, 0, 0, 0, 0, 0, \"\"]\n");	// no comma
  fprintf(f, "]}\n");
}

// Return true if the event is mark_a mark_b mark_c
inline bool is_mark_abc(uint64 event) {return (event == 0x020A) || (event == 0x020B) || (event == 0x020C);}

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
  fprintf(stderr, "Usage: spantotrim label | start_sec [stop_sec]\n");
  exit(0);
}

//
// Filter from stdin to stdout
//
int main (int argc, const char** argv) {
  double start_sec = 0.0;
  double stop_sec = 999.0;
  char label[8];
  char notlabel[8];
  // Default: label filter is a nop
  bool inside_label_span = true;
  bool next_inside_label_span = true;

  if (argc < 2) {Usage();}
  
  if ('9' < argv[1][0]) {
    // Does not start with a digit. Assume it is a label and
    // that we should filter  
    //   Mark_abc label .. Mark_abc /label 
    // inclusive
    int len = strlen(argv[1]);
    if (len > 6 ) {len = 6;}
    memcpy(label, argv[1], len + 1);
    memcpy(notlabel + 1, label, len);
    notlabel[0] = '/';
    notlabel[7] = '\0';
    inside_label_span = false;
    next_inside_label_span = false;
  }

  if (inside_label_span && (argc >= 2)) {
    int n = sscanf(argv[1], "%lf", &start_sec);
    if (n != 1) {Usage();}
  }
  if (inside_label_span && (argc >= 3)) {
    int n = sscanf(argv[2], "%lf", &stop_sec);
    if (n != 1) {Usage();}
  }

  // expecting:
  //    ts           dur       cpu  pid  rpc event arg ret  name--------------------> 
  //  [ 22.39359781, 0.00000283, 0, 1910, 0, 67446, 0, 256, "gnome-terminal-.1910"],

  int output_events = 0;
  char buffer[kMaxBufferSize];
  while (ReadLine(stdin, buffer, kMaxBufferSize)) {
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
    if (onespan.start_ts >= 999.0) {break;}	// Always strip 999.0 end marker and stop
    if (onespan.start_ts < start_sec) {continue;}
    if (onespan.start_ts >= stop_sec) {continue;}

    // Keep an eye out for mark_abc
    if (is_mark_abc(onespan.event)) {
      char temp[8];
      Base40ToChar(onespan.arg, temp);
      // Turn on keeping events if we find a mathcing label
      if (strcmp(label, temp) == 0) {inside_label_span = true;}
      // Defer turning off keeping events so we keep this one
      next_inside_label_span = inside_label_span;
      if (strcmp(notlabel, temp) == 0) {next_inside_label_span = false;}
    }
    if (!inside_label_span) {continue;}	

    // Name has trailing punctuation, including ],
    fprintf(stdout, "[%12.8f, %10.8f, %d, %d, %d, %d, %d, %d, %d, %s\n",
            onespan.start_ts, onespan.duration,
            onespan.cpu, onespan.pid, onespan.rpcid, onespan.event, 
            onespan.arg, onespan.retval, onespan.ipc, onespan.name);
    ++output_events;

    inside_label_span = next_inside_label_span;
  }

  // Add marker and closing at the end
  FinalJson(stdout);
  fprintf(stderr, "spantotrim: %d events\n", output_events);

  return 0;
}
