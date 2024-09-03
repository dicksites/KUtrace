// Sample mystery program to load up floating-point execution units. 
// Runs on/off ~four times per secfond for a minute
// Copyright 2021 Richard L. Sites
//
// kutrace version. This does  not start or stop tracing, so can run multiple ones
//
// Usage: fdiv_hog [n]
//         n msec between iterations. Defaults to 200
// Compile with
//   g++ -O2 flt_hog.cc kutrace_lib.cc -o flt_hog
//

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "kutrace_lib.h"
#include "timecounters.h"

static const int kIterations = 1000 * 1000 * 1;	// About 10 msec

static int msec_wait = 20;

// Sleep for n milliseconds
void msleep(int msec) {
  struct timespec ts;
  ts.tv_sec = msec / 1000;
  ts.tv_nsec = (msec % 1000) * 1000000;
  nanosleep(&ts, NULL);
}

double DoIterations(int n, double start_divd) {
  double divd1 = start_divd;
  double divd2 = start_divd;
  double prod1 = start_divd;
  double prod2 = start_divd;
  double sum1 = 0.0;
  double sum2 = 0.0;
  for (int i = 0; i < n; ++i) {
    // Mark every 4096 iterations, so we can see how time changes
    if ((i & 0x0fff) == 0) {
      kutrace::mark_d(i >> 10);
    }
    sum1 += prod1;
    sum2 += divd1;
    prod1 *= 1.000000001;
    divd1 /= 1.000000001;
    sum1 -= prod2;
    sum2 -= divd2;
    prod2 *= 0.999999999;
    divd2 /= 0.999999999;
  }
  return divd1 + prod1 +  divd2 + prod2 + sum1 + sum2;
}

double DoIterations2(int n, double start_divd) {
  double divd1 = start_divd;
  double divd2 = start_divd;
  double divd3 = start_divd;
  double divd4 = start_divd;
  for (int i = 0; i < n; ++i) {
    // Mark every 4096 iterations, so we can see how time changes
    if ((i & 0x0fff) == 0) {
      kutrace::mark_d(i >> 10);
    }
    divd1 /= 1.000000001;
    divd2 /= 0.999999999;
    divd3 /= 1.000000002;
    divd4 /= 0.999999998;

    divd1 /= 0.999999999;
    divd2 /= 1.000000001;
    divd3 /= 0.999999998;
    divd4 /= 1.000000002;
  }
  return divd1 + divd2 + divd3 + divd4;
}


int main (int argc, const char** argv) {
  if (1 < argc) {msec_wait = atoi(argv[1]);}

  uint64_t startcy, stopcy;

  double divd = 123456789.0;
  startcy = GetCycles();
  divd = DoIterations2(kIterations, divd);
  stopcy = GetCycles();
  int64_t elapsed = stopcy - startcy;	// Signed to avoid compiled code for negative unsigned
  double felapsed = elapsed;


  // Run for 1 minute approximately if 20ms wait
  for (int i = 0; i < 60*30; ++i) {
    divd = DoIterations2(kIterations * 2, divd);
    msleep(msec_wait);
  }
  
  fprintf(stdout, "%d iterations, %lu cycles, %4.2f cycles/iteration\n", 
          kIterations, elapsed, felapsed / kIterations);
  fprintf(stdout, "%f\n", divd);	// Make divd live
  return 0;
}
