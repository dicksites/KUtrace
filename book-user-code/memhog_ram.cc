// OverFill 20MB of L3 cache infinite loop
// Copyright 2021 Richard L. Sites
//
// compile with g++ -O2 memhog_ram.cc kutrace_lib.cc -o memhog_ram
//

#include <stdio.h>
#include <stdlib.h>		// atoi
#include <time.h>		// nanosleep

#include "basetypes.h"
#include "kutrace_lib.h"

static const int kSize = 5 * 512 * 1024;	// 20MB, count of 8-byte U64s

static int msec_wait = 20;

// Sleep for n milliseconds
void msleep(int msec) {
  struct timespec ts;
  ts.tv_sec = msec / 1000;
  ts.tv_nsec = (msec % 1000) * 1000000;
  nanosleep(&ts, NULL);
}

static const int kIterations = 60 * 1000;	// NOTE: 60K ~5msec per iter
double fdiv_wait(int iter) {
  double divd = 123456789.0;
  for (int k = 0; k < iter; ++k) {
    for (int i = 0; i < kIterations; ++i) {
      divd /= 1.0001;
      divd /= 0.9999;
    }
  }
  return divd;	// Make live
}


int main (int argc, const char** argv) {
  if (1 < argc) {msec_wait = atoi(argv[1]);}
  fprintf(stdout, "Starting memory RAM hog.\n");

  uint64* buffer = new uint64[kSize];
  for (int i = 0; i < kSize; ++i) {buffer[i] = i;}

  uint64 sum = 0;
  for (int k = 0; k < 10000; ++k) {	// Approx 30 seconds
    for (int i = 0; i < kSize; ++i) {
      sum += buffer[i];
      buffer[i] = sum;
    }
    if ((k & 3) == 0) {kutrace::mark_d(k);}

    // Wait for ~20 msec between every 10 passes
    if ((k % 10) == 0) {msleep(msec_wait);}
  }

  printf("sum %llu\n", sum);	// Make live
  return 0;
}

