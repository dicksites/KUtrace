// Fill 128KB of L2 cache infinite loop
// Copyright 2021 Richard L. Sites

#include <stdio.h>
#include <time.h>		// nanosleep

#include "basetypes.h"
#include "kutrace_lib.h"

static const int kSize = 32 * 1024;	// count of 8-byte U64s

// Sleep for n milliseconds
void msleep(int msec) {
  struct timespec ts;
  ts.tv_sec = msec / 1000;
  ts.tv_nsec = (msec % 1000) * 1000000;
  nanosleep(&ts, NULL);
}

static const int kIterations = 70 * 1000;	// NOTE: 70K ~1msec per iter
double fdiv_wait(int iter) {
  double divd = 123456789.0;
  for (int k = 0; k < iter; ++k) {
    for (int i = 0; i < kIterations; ++i) {
      divd /= 1.0001;
      divd /= 0.9999;
    }
  }
  kutrace::mark_d(666);
  return divd;	// Make live
}


int main (int argc, const char** argv) {
  fprintf(stdout, "Starting memory L2 hog.\n");
  uint64* buffer = new uint64[kSize];
  
  uint64 sum = 0;
  for (int k = 0; k < 100000000; ++k) {
    for (int i = 0; i < kSize; ++i) {
      sum += buffer[i];
      buffer[i] = sum;
    }
    kutrace::mark_d((k & 255) + 2000);

    // Wait for 1 msec between every 4 passes
    // if ((k & 3) == 0) {msleep(1);}
    if ((k & 3) == 0) {fdiv_wait(1);}
  }

  printf("sum %llu\n", sum);	// Make live
  return 0;
}

