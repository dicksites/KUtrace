// Little program to read disk/SSD continuously 
// against it and observe the interference.
//
// Copyright 2021 Richard L. Sites
//
// Design goals: Run for about 60 seconds reading continuously
//

// Usage: disk_readhog <file name on desired disk/SSD> [MB to create]
// Compile with 
//   g++ -O2 disk_readhog.cc -o disk_readhog

#include <errno.h>
#include <fcntl.h>		// open
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>		// open
#include <sys/time.h>		// gettimeofday
#include <sys/types.h>		// lseek
#include <time.h>
#include <unistd.h>		// lseek

#include "basetypes.h"
#include "polynomial.h"

//static const size_t kReadBlockSize = 64 * 1024;	// Read 64KB at a time
//static const int kBlocksPerMB = 16;
static const size_t kReadBlockSize = 256 * 1024;	// Read 256KB at a time
static const int kBlocksPerMB = 4;

// Return current time of day as seconds and fraction since January 1, 1970
double GetSec() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv.tv_sec + (tv.tv_usec / 1000000.0);
}

// ---- For creating a test file ----
// Pad a string out to length using pseudo-random characters.
//  x is a pseudo-random seed and is updated by this routine
//  s is the input character string to be padded and must be allocated big enough
//    to hold at least length characters
//  curlen is the current length of s, bytes to be retained
//  padded_len is the desired new character length
// If curlen >= padded_len, s is returned unchanged. Else it is padded.
// Returns s in both cases.
// DOES NOT return a proper c string with trailing zero byte
char* PadTo(uint32* x, char* s, int curlen, int padded_len) {
  char* p = s + curlen;	  // First byte off the end;
  for (int i = 0; i < (padded_len - curlen); ++i) {
    if ((i % 5) == 0) {
      *p++ = '_';
    } else {
      *p++ = "abcdefghijklmnopqrstuvwxyz012345"[*x & 0x1f];
      *x = POLYSHIFT32(*x);
    }
  }
  return s;
}

// Creates a pseudo-random 1MB buffer and then writes it multiple times.
void MakeTestFile(const char* fname, int sizeinmb) {
  char* temp = (char*)malloc(1024 * 1024);
  FILE* f = fopen(fname, "wb");
  if (f == NULL) {fprintf(stderr, "%s did not open\n", fname); return;}

  uint32 randseed = POLYINIT32;
  for (int i = 0; i < sizeinmb; ++i) {
    PadTo(&randseed, temp, 0, 1024 * 1024);
    fwrite(temp, 1, 1024 * 1024, f);
  }

  fclose(f);
  free(temp);
}
// ---- End For creating a test file ---- 

int main(int argc, const char** argv) {
  // Get started
  const char* filename = argv[1];
  if (argc > 2) {
    // If extra arg, create test file of that many MB and exit
    int mb_to_create = atoi(argv[2]);
    MakeTestFile(filename, mb_to_create);
    fprintf(stderr, "%dMB written to %s\n", mb_to_create, filename); 
    return 0;
  }

  if (argc < 2) {
    fprintf(stderr, "Usage: disk_readhog <file name on desired disk/SSD> [MB to create]\n");
    return 0;
  }

  char* buffer = (char*)malloc(kReadBlockSize + 4096);
  char* aligned_buffer = (char*)((long unsigned int)buffer & ~0xFFFlu);
  double total_start, total_elapsed;
  int fd = open(filename, O_RDONLY | O_DIRECT);
  if (fd < 0) {perror("disk_readhog open"); return 0;}

  total_start = GetSec();
  int block_count = 0;
  ssize_t n = 0;
  // Loop for 60 seconds
  while (GetSec() < (total_start + 60.0)) {
    lseek(fd, 0, SEEK_SET);
    while ((n = read(fd, aligned_buffer, kReadBlockSize)) > 0) {++block_count;}
    if (n < 0) {perror("disk_readhog read"); return 0;}
  }

  // All done
  total_elapsed = GetSec() - total_start;
  int mb_read = block_count / kBlocksPerMB;
  fprintf(stdout, "Elapsed time for %dMB %5.3f sec = %4.1fMB/sec\n", 
          mb_read, total_elapsed, mb_read / total_elapsed);

  close(fd);
  return 0;
}

