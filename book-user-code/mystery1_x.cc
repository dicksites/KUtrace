// Sample mystery program to measure how long an add takes. Flawed.
// Copyright 2021 Richard L. Sites

#include <stdint.h>
#include <stdio.h>
#include <time.h>		// time()
#include "timers.h"

static const int kIterations = 1000 * 1000000;

int main (int argc, const char** argv) { 
  uint64_t sum = 0;

  time_t t = time(NULL);	// A number that the compiler does not know
  int incr = t & 255;		// Unknown increment 0..255

  int64_t startcy = GetCounter();
  for (int i = 0; i < kIterations; ++i) {
    sum += incr;
  }
  int64_t elapsed = GetCounter() - startcy;
  double felapsed = elapsed;

  fprintf(stdout, "%d iterations, %lu cycles, %4.2f cycles/iteration\n", 
          kIterations, elapsed, felapsed / kIterations);
  
  // fprintf(stdout, "%lu %lu\n", t, sum);	// Make sum live

  return 0;
}
