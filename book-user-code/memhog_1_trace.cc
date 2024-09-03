// Fill 32KB L1 cache infinite loop
// Copyright 2021 Richard L. Sites

// SHORT INSTRUMENTED VERSION

#include <stdio.h>
#include <string.h>

#include "basetypes.h"
#include "dclab_trace_lib.h"

static const int kSize = 8 * 1024;	// count of 8-byte U64s

int main (int argc, const char** argv) {
  fprintf(stdout, "Starting instrumented memory L1 hog.\n");
  uint64* buffer = new uint64[kSize];

  //Exit immediately if the module is not loaded
  if (!dclab_trace::test()) {
    fprintf(stderr, "FAIL, module dclab_trace_mod.ko not loaded\n");
    return 0;
  }

  // Executable image name, backscan for slash if any
  const char* slash = strrchr(argv[0], '/');
  dclab_trace::go((slash == NULL) ? argv[0] : slash + 1);

  uint64 sum = 0;
  for (int k = 0; k < 40000; ++k) {
    for (int i = 0; i < kSize; ++i) {
      sum += buffer[i];
      buffer[i] = sum;
    }
    // Each pass is just 3-4 usec, so we get a lot of chatter marking every pass.
    // Just do every fourth pass.
    if (true || (k & 3) == 0) {dclab_trace::mark_d((k & 255) + 1000);}
  }

  fprintf(stderr, "memhog_1_trace.trace written\n");
  dclab_trace::stop("memhog_1_trace.trace");	// DESIGN BUG: exits

  printf("sum %lu\n", sum);
  return 0;
}

