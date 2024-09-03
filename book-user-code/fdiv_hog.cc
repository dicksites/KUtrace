// Sample mystery program to measure how long an FDIV takes. 
// Runs on/off ~four times per second for a minute
// Copyright 2021 Richard L. Sites
//
// kutrace version. This does  not start or stop tracing, so can run multiple ones
//
// Usage: fdiv_hog [n]
//         n msec between iterations. Defaults to 200
// Compile with
//   g++ -O2 fdiv_hog.cc kutrace_lib.cc -o fdiv_hog
//
// Postprocess with
//   cat /tmp/fdiv101.trace |./rawtoevent |sort -n |./eventtospan "fdiv101" |sort |./spantotrim 0 |./spantospan 0 >/home/public/time_fdiv101.json

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <x86intrin.h>

#include "kutrace_lib.h"

static const int kIterations = 1000 * 1000 * 1;	// About 5 msec

static int msec_wait = 200;

// Sleep for n milliseconds
void msleep(int msec) {
  struct timespec ts;
  ts.tv_sec = msec / 1000;
  ts.tv_nsec = (msec % 1000) * 1000000;
  nanosleep(&ts, NULL);
}

double DoIterations(int n, double start_divd) {
  double divd = start_divd;
  for (int i = 0; i < n; ++i) {
    // Mark every 4096 iterations, so we can see how time changes
    // Also reset dividend so we don't underflow
    if ((i & 0x0fff) == 0) {
      kutrace::mark_d(i >> 10);
      divd = start_divd;
    }
    divd /= 1.000001;
    divd /= 1.000000001;
  }
  return divd;
}


int main (int argc, const char** argv) {
  if (1 < argc) {msec_wait = atoi(argv[1]);}

  uint64_t startcy, stopcy;

  double divd = 123456789.0;
  startcy = __rdtsc();
  divd = DoIterations(kIterations, divd);
  stopcy = __rdtsc();
  int64_t elapsed = stopcy - startcy;	// Signed to avoid compiled code for negative unsigned
  double felapsed = elapsed;


  // Run for 1 minute approximately
  for (int i = 0; i < 60*4; ++i) {
    divd = DoIterations(kIterations * 10, divd);
    msleep(msec_wait);
  }
  
  fprintf(stdout, "%d iterations, %lu cycles, %4.2f cycles/iteration\n", 
          kIterations, elapsed, felapsed / kIterations);
  fprintf(stdout, "%f\n", divd);	// Make divd live
  return 0;
}
