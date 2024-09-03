// Fill 32KB L1 cache infinite loop
// Copyright 2021 Richard L. Sites

#include <stdio.h>
#include "basetypes.h"
#include "dclab_trace_lib.h"

static const int kSize = 7 * 1024;

int main (int argc, const char** argv) {
  fprintf(stdout, "Starting memory L1 hog.\n");
  uint64* buffer = new uint64[kSize];
  
  uint64 sum = 0;
  for (int k = 0; k < 100000000; ++k) {
    for (int i = 0; i < kSize; ++i) {
      sum += buffer[i];
    }
    // Each pass is just 3-4 usec, so we get a lot of chatter marking every pass.
    // Just do every fourth pass.
    if ((k & 3) == 0) {
      dclab_trace::mark_d((k & 255) + 1000);
    }
  }

  printf("sum %lu\n", sum);
  return 0;
}

