// Little program to fil up memory and take page faults
// Copyright 2021 Richard L. Sites
//
// compile with g++ -O2 paging_hog.cc kutrace_lib.cc -o paging_hog

#include "stdlib.h"
#include "stdio.h"

#include "basetypes.h"
#include "kutrace_lib.h"
#include "polynomial.h"

//static const int64 kMAX_MB = 1000;	//  1 GB
//static const int64 kMAX_MB = 5000;	//  5 GB
static const int64 kMAX_MB = 8000;	//  8 GB
//static const int64 kMAX_MB = 10000;	// 10 GB

static const int64 k40_MB = 40 * 1024 * 1024;

int main (int argc, const char** argv) {
  // Make an array to hold all the pointers
  int64 chunkcount = (kMAX_MB << 20) / k40_MB;
  char** chunks = (char**)malloc(chunkcount * sizeof(char*));

  // Allocate chunks of 40MB until malloc fails, then back off one
  fprintf(stdout, "Allocating up to %lld MB in %lld 40MB chunks\n", kMAX_MB, chunkcount);
  uint32 x = POLYINIT32;
  int64 chunklimit = 0;
  for (int64 i = 0; i < chunkcount; ++i) {
    chunklimit = i;
    chunks[i] = (char*)malloc(k40_MB);
    if (chunks[i] == NULL) {
      // Allocation failed
      fprintf(stdout, "Allocation of chunk %lld failed\n", i);
      if (i == 0) {return 0;}	// No room at all
      // Make a little room by freeing the last successful 40MB
      free(chunks[i - 1]);
      chunklimit = i - 1;
      break;
    }
    // We got a chunk. Write to each page so they are not the single all-zero page.
    // We only need to touch one byte of each page to force a real allocation.
    fprintf(stdout, ".");
    if ((i % 25) == 24) {fprintf(stdout, "\n");}
    kutrace::mark_d(i);
    char* ptr = chunks[i];
    for (int k = 0; k < k40_MB; k += (1 << 12)) {
      ptr[k] = (char)x;
      x = POLYSHIFT32(x);
    }
  }

  // Scan the allocated area, creating ~1M page faults to/from disk
  fprintf(stdout, "Scanning %lld 40MB chunks\n", chunklimit);
  for (int64 i = 0; i < chunklimit; ++i) {
    char* ptr = chunks[i];
    for (int k = 0; k < k40_MB; k += (1 << 12)) {
      ptr[k] = (char)x;
      x = POLYSHIFT32(x);
    }
  }

  return 0;
}

