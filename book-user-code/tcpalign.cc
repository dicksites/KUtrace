// Little program to time-align a TCP dump trace with a KUtrace
// Copyright 2021 Richard L. Sites
//
// Inputs:
//  foo_pcap.json file derived from a tcpdump trace via pcaptojson
//  ku_foo.json file created by eventtospan, containing RPCs
//
// Method:
//  The pcap file has thousands of RPC message events with tcpdump timestamps,
//  which unfortunately are not from the same time base as user-mode gettimeofday
//  used in KUtrace. 
//
//  So we look for syswrites in the KUtrace that are within an RPC response
//  time range, and look for the corresponding message by RPCID in the tcpdump
//  trace. Near the beginning of the traces and near the end, we calculate the
//  time offset that when added to the tcpdump times will place the outbound 
//  message on the wire about 2 usec after the start of the write(). If there 
//  is drift between the time bases, the early and late offsets will differ 
//  somewhat. Responses more closely align between write and wire than requests
//  between wire and read.
//
//  In addition, the KUtrace file has timestmaps starting at a minute boundary
//  as reflected by the basetime. The tcpdump file in general may start 
//  during a different minute. To get nearby, we assume that the traces are
//  time aligned withn +/- 30 seconds of each other.
//
//  RPCIDs are only 16 bits, so there will be duplicates in many traces. We 
//  match up ones that have the smallest absolute time difference.
// 
//  The two traces will in general only partially overlap, so we search initally
//  for an RPCID near the beginning of either that is also found in the other.
//  If they overlap at all (and no data is missing), either the first RPC in the 
//  KUtrace will be in the tcpdump trace, or the first RPC in the tcptrace will 
//  be in the KUtrace.

#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <string>

#include "basetypes.h"
#include "kutrace_lib.h"

using std::string;

typedef struct {
  double start_ts;	// Multiples of 10 nsec
  double duration;	// Multiples of 10 nsec
  int cpu;
  int pid;
  int rpcid;
  int eventnum;
  int arg;
  int retval;
  int ipc;
  string name;
} OneSpan;


typedef struct {
  uint32 rpcid;
  double ts;
} RpcTime;


static const int kMaxBufferSize = 256;

static const int write_event = 2049;	// Depends on which Linux, see
					// kutrace_control_names.h
static const int rx_pkt_event = KUTRACE_RPCIDRXMSG;
static const int tx_pkt_event = KUTRACE_RPCIDTXMSG;

// We expect the the outbound message to be about 5 usec after the start of the syswrite
static const double write_to_rpc_delay = 0.000005;

// We expect a good fit to have the new KUtrace - tcpdump difference to be +/- 100 usec
static const double max_fitted_diff = 0.000100;

typedef struct {
  double x;
  double y;
} XYPair;

typedef struct {
  double x0;
  double y0;
  double slope;
} Fit;

void print_fit(FILE* f, const Fit& fit) {
  fprintf(f, "Fit: x0 %10.6f, y0 %10.6f, slope %12.8f\n", fit.x0, fit.y0, fit.slope);
}

// Calculate a least-squares fit
// Input is an array of k  <x,y> pairs
// Output is x0, y0, and slope such that y = ((x - x0) * slope) + y0
void get_fit(const XYPair* xypair, int k, Fit* fit)  {
  // Default to the identity mapping
  fit->x0 = 0.0;
  fit->y0 = 0.0;
  fit->slope = 1.0;
  if (k <= 0) {return;}

  double n = 0.0;
  double nx = 0.0;
  double ny = 0.0;
  double nxy = 0.0;
  double nxx = 0.0;

  // Avoid precision loss from squaring big numbers by offsetting from first value
  double xbase = xypair[0].x;

  // Now calculate the fit
  for (int i = 0; i < k; ++i) {
    double xi = xypair[i].x - xbase;
    double yi = xypair[i].y;
    n += 1.0;
    nx += xi;
    ny += yi;
    nxy += xi * yi;
    nxx += xi * xi;
  }
  double num = (n * nxy) - (nx * ny);
  double denom = (n * nxx) - (nx * nx);

  fit->x0 = xbase;
  if (denom != 0.0) {
    fit->slope = num / denom;
  } else {
    fit->slope = 1.0;
  }
  fit->y0 = ((ny * nxx) - (nx * nxy)) / denom;
}

inline double remap(double x, const Fit& fit) {return ((x - fit.x0) * fit.slope) + fit.y0;}



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

string strip_suffix(const string& s) {
  size_t period = s.find_last_of('.');
  return s.substr(0, period);
}

// Expecting hh:mm:ss in s
int get_seconds_in_day(const char* s) {
  int hr  = atoi(&s[0]);
  int min = atoi(&s[3]);
  int sec = atoi(&s[6]);
  return (hr * 3600) + (min * 60) + sec;
}

void usage() {
  fprintf(stderr, "usage: tcpalign <kutrace json filename> <tcpdump json filename>\n");
  exit(0);
}

int main (int argc, const char** argv) {
  // Open files or die

  // Read all the RPC events in KUtrace, recording time w/tracebase of first 
  //   instance of each response write RPCID.

  // Read all the RPC events in tcpdump, recording time w/tcpbase of first 
  //   instance of each response RPC.

  // Scan matching pairs to calculate kutrace - tcpdump time difference
  // Maybe Throw out big differences
  // Do least-squares fit; print offset and slope w.r.t. first matching RPC

  // Rewrite the tcpdump file

  if (argc <= 2) {usage();}
  const char* kutrace_name = argv[1];
  const char* tcpdump_name = argv[2];
  string out_name = strip_suffix(string(tcpdump_name)) + "_align.json";

  FILE* ku = fopen(kutrace_name, "r");
  if (ku == NULL) {fprintf(stderr, "%s did not open\n", kutrace_name); exit(0);}
  FILE* tcp = fopen(tcpdump_name, "r");
  if (tcp == NULL) {fprintf(stderr, "%s did not open\n", tcpdump_name); exit(0);}

  RpcTime ku_rpc[65536];
  RpcTime tcp_rpc[65536];
  memset(ku_rpc, 0, 65536 * sizeof(RpcTime));
  memset(tcp_rpc, 0, 65536 * sizeof(RpcTime));
 
  char buffer[kMaxBufferSize];
  char ku_basetime_str[kMaxBufferSize];
  char tcp_basetime_str[kMaxBufferSize];
  double ku_basetime = 0.0;	// Seconds within a day
  double tcp_basetime = 0.0;	// Seconds within a day

  OneSpan span;
  char name_buffer[256];

  // Expecting either (note leading space)
  // 0123456789.123456789.123456789.123456789
  //  "tracebase" : "2020-08-28_14:18:00",
  //  "tcpdumpba" : "2020-08-28_14:18:00",
  // KUTRACE_RPCIDRXMSG, event 516:
  // [  0.00003500, 0.00000001, 0, 0, 12267, 516, 0, 0, 0, "rpc.12267"],
  // KUTRACE_RPCIDTXMSG, event 517:
  // [  0.00017900, 0.00000001, 0, 0, 39244, 517, 4012, 0, 0, "rpc.39244"],
  // syswrite, event 2049, rpcid 52790 or whatever:
  // [ 56.25728887, 0.00001500,    1, 11903, 52790, 2049, 4, 4100, 1, "write"],
  //   ts         1 dur       2 CPU 3 pid  4 rpc  5 event

  // Read the kutrace times
  //--------------------------------------------------------------------------//
  while (ReadLine(ku, buffer, kMaxBufferSize)) {
    if (memcmp(buffer, " \"tracebase\"", 12) == 0) {
      memcpy(ku_basetime_str, buffer, kMaxBufferSize);
      int temp = get_seconds_in_day(&buffer[27]);
fprintf(stderr, "ku_basetime  = %s\n", buffer); 
fprintf(stderr, "ku_basetime  = %02d:%02d:%02d\n", temp/3600, (temp/60)%60, temp%60); 
      ku_basetime = temp;
      continue;
    }
    if (buffer[0] != '[') {continue;}

    int n = sscanf(buffer, "[%lf,%lf,%d,%d,%d,%d,%d,%d,%d,%s",
                   &span.start_ts, &span.duration, 
                   &span.cpu, &span.pid, &span.rpcid, 
                   &span.eventnum, &span.arg, &span.retval, &span.ipc, 
                   name_buffer);
    if (n != 10) {continue;}

    if (span.eventnum == write_event) {
      if (ku_rpc[span.rpcid].ts == 0.0) {	// first time only
        ku_rpc[span.rpcid].ts = span.start_ts;
fprintf(stdout, "ku_rpc[%d] = %8.6f + %8.6f\n", span.rpcid, span.start_ts, ku_basetime);
      }
    }
  }
  fclose(ku);

  // Read the tcpdump times
  //--------------------------------------------------------------------------//
  while (ReadLine(tcp, buffer, kMaxBufferSize)) {
    if (memcmp(buffer, " \"tcpdumpba\"", 12) == 0) {
      memcpy(tcp_basetime_str, buffer, kMaxBufferSize);
      int temp = get_seconds_in_day(&buffer[27]);
fprintf(stderr, "tcp_basetime = %s\n", buffer); 
fprintf(stderr, "tcp_basetime = %02d:%02d:%02d\n", temp/3600, (temp/60)%60, temp%60); 
      tcp_basetime = temp;
      continue;
    }
    if (buffer[0] != '[') {continue;}

    int n = sscanf(buffer, "[%lf,%lf,%d,%d,%d,%d,%d,%d,%d,%s",
                   &span.start_ts, &span.duration, 
                   &span.cpu, &span.pid, &span.rpcid, 
                   &span.eventnum, &span.arg, &span.retval, &span.ipc, 
                   name_buffer);
    if (n != 10) {continue;}

    if (span.eventnum == tx_pkt_event) {
      if (tcp_rpc[span.rpcid].ts == 0.0) {	// first time only
        tcp_rpc[span.rpcid].ts = span.start_ts;
fprintf(stdout, "tcp_rpc[%d] = %8.6f + %8.6f\n", span.rpcid, span.start_ts, tcp_basetime);
      }
    }
  }
  fclose(tcp);

  // See what we have
  //--------------------------------------------------------------------------//
  bool fail = false;
  if (ku_basetime == 0.0) {
    fprintf(stderr, "kutrace has no basetime\n");
    fail = true;
  }
  if (tcp_basetime == 0.0) {
    fprintf(stderr, "tcpdump has no basetime\n");
    fail = true;
  }
  if (600 < abs(ku_basetime - tcp_basetime)) {
    fprintf(stderr, "kutrace and tcpdump basetimes differ by more than 10 minutes:\n");
    fprintf(stderr, "  kutrace %s\n", ku_basetime_str);
    fprintf(stderr, "  tcpdump %s\n", tcp_basetime_str);
    fail = true;
  } 
  if (fail) {exit(0);}

  // Map tcp times to 5 usec after the ku write() starting times
  //--------------------------------------------------------------------------//
  int k = 0;
  XYPair pair[65536];
  for (int rpcid = 0; rpcid < 65536; ++rpcid) {
    if ((ku_rpc[rpcid].ts != 0.0) && (tcp_rpc[rpcid].ts != 0.0)) {
      // Map tpc time to the ku start minute
      pair[k].x = tcp_rpc[rpcid].ts + (ku_basetime - tcp_basetime);
      // Record the incoming ku-tcp offset from write + 5 usec
      pair[k].y = (ku_rpc[rpcid].ts + write_to_rpc_delay) - tcp_rpc[rpcid].ts;
fprintf(stdout, "  [%d] diffs[%d] = %8.6f (%8.6f - %8.6f)\n", k, rpcid, pair[k].y, ku_rpc[rpcid].ts + write_to_rpc_delay, pair[k].x);
      ++k;
    }
  }
  fprintf(stderr, "%d pair matches found\n", k);

  // Fit #1
  //--------------------------------------------------------------------------//
  Fit fit;
  get_fit(pair, k, &fit);
  print_fit(stderr, fit);

  // Fit #2
  //--------------------------------------------------------------------------//
  // The fit may well be biased by outlier (typically late packet transmission)
  // times, so redo the fit chopping off anything beyond +/- 100 usec
  int k2 = 0;
  XYPair pair2[65536];
  for (int kk = 0; kk < k; ++kk) {
    // if remap moves original offset to perfect alignment, diff = 0;
    double diff = pair[kk].y - remap(pair[kk].x, fit);
    if (max_fitted_diff < fabs(diff)) {continue;}	// Too far away; ignore
    pair2[k2++] = pair[kk];
  }
  fprintf(stderr, "%d pair2 matches found\n", k2);

  // If we retained at least half the points, re-fit, else leave it alone
  if (k <= (k2 * 2)) {
    get_fit(pair2, k2, &fit);
    print_fit(stderr, fit);
  }

  // Write the old and new offsets for a json file
  //--------------------------------------------------------------------------//
  for (int kk = 0; kk < k; ++kk) {
    fprintf(stdout, "[%10.6f, %f, %f],\n", pair[kk].x, pair[kk].y, remap(pair[kk].x, fit));
  }

  // Read the tcp file and write new aligned one
  //--------------------------------------------------------------------------//
  FILE* tcp2 = fopen(tcpdump_name, "r");
  if (tcp2 == NULL) {fprintf(stderr, "%s did not open\n", tcpdump_name); exit(0);}

  FILE* out = fopen(out_name.c_str(), "w");
  if (out == NULL) {fprintf(stderr, "%s did not open\n", out_name.c_str()); exit(0);}

  // Read the tcpdump times, remap, and write aligned file
  while (ReadLine(tcp2, buffer, kMaxBufferSize)) {
    // Copy everything that is not a span
    if (buffer[0] != '[') {
      fprintf(out, "%s\n", buffer);
      continue;
    }

    int n = sscanf(buffer, "[%lf,%lf,%d,%d,%d,%d,%d,%d,%d,%s",
                   &span.start_ts, &span.duration, 
                   &span.cpu, &span.pid, &span.rpcid, 
                   &span.eventnum, &span.arg, &span.retval, &span.ipc, 
                   name_buffer);
    if (n != 10) {
      fprintf(out, "%s\n", buffer);
      continue;
    }

    // Align the time, except for the 999.0 end marker
    if (span.start_ts != 999.0) {
      double old_ts = span.start_ts;
      span.start_ts += remap(span.start_ts, fit);
//fprintf(stdout, "[%d] %f ==> %f (%f)\n", span.rpcid, old_ts, span.start_ts, adjust_offset);
//fprintf(stdout, [%f, %f, %f],\n", old_ts, span.start_ts, adjust_offset);

    }

    fprintf(out, "[%12.8f, %10.8f, %d, %d, %d, %d, %d, %d, %d, %s\n",
                   span.start_ts, span.duration, 
                   span.cpu, span.pid, span.rpcid, 
                   span.eventnum, span.arg, span.retval, span.ipc, 
                   name_buffer);
  }
  fclose(tcp2);
  fclose(out);

  fprintf(stderr, "  %s written\n", out_name.c_str());
  return 0;
}


