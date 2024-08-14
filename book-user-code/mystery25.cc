// Little program to time disk transfers
// Copyright 2021 Richard L. Sites

// Usage: mystery25 <file name on desired disk>
// Compile with 
//   g++ -O2 mystery25.cc kutrace_lib.cc  -o mystery25

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>		// gettimeofday
#include <sys/types.h>		// lseek
#include <time.h>
#include <unistd.h>		// lseek

#include "basetypes.h"
#include "kutrace_lib.h"
#include "polynomial.h"
#include "timecounters.h"

// Time on a slow 5400 rpm disk
// $ ./mystery25 /tmp/myst25.bin
// opening /tmp/myst25.bin for write
//   write:     40.000MB  0.008sec 4741.022MB/sec
//   sync:      40.000MB  0.836sec 47.835MB/sec
//   read:      40.000MB  0.673sec 59.466MB/sec
//   seq read:  40.000MB  1.470sec 27.203MB/sec
//   rand read: 40.000MB 68.849sec  0.581MB/sec

// Time on a cheap SSD
// $ ./mystery25 /datassd/dserve/myst25.bin
// opening /datassd/dserve/myst25.bin for write
//   write:     40.000MB  0.013sec 3030.303MB/sec
//   sync:      40.000MB  0.068sec 587.070MB/sec
//   read:      40.000MB  0.057sec 706.003MB/sec
//   seq read:  40.000MB  0.548sec 72.947MB/sec
//   rand read: 40.000MB  0.909sec 43.985MB/sec


// Time on two disk files simultaneously
// $ ./mystery25 /tmp/myst25.bin & ./mystery25 /tmp/myst25a.bin
// [2] 3306
// opening /tmp/myst25.bin for write
// opening /tmp/myst25a.bin for write
//   write:     40.000MB  0.009sec 4236.839MB/sec
//   write:     40.000MB  0.019sec 2158.312MB/sec
//   sync:      40.000MB  1.762sec 22.704MB/sec
//   sync:      40.000MB  1.753sec 22.822MB/sec
//   read:      40.000MB  1.498sec 26.701MB/sec
//   read:      40.000MB  1.525sec 26.234MB/sec
//   seq read:  40.000MB  3.087sec 12.958MB/sec
//   seq read:  40.000MB  3.276sec 12.210MB/sec
//   rand read: 40.000MB 151.276sec  0.264MB/sec
// dsites@dclab-2:~/code$   rand read: 40.000MB 151.074sec  0.265MB/sec

// $ ./mystery25 /tmp/myst25.bin & ./mystery25 /tmp/myst25a.bin
// opening /tmp/myst25.bin for write
// opening /tmp/myst25a.bin for write
//   write:     40.000MB  0.009sec 4236.839MB/sec
//   write:     40.000MB  0.019sec 2158.312MB/sec
//   sync:      40.000MB  1.762sec 22.704MB/sec
//   sync:      40.000MB  1.753sec 22.822MB/sec
//   read:      40.000MB  1.498sec 26.701MB/sec
//   read:      40.000MB  1.525sec 26.234MB/sec
//   seq read:  40.000MB  3.087sec 12.958MB/sec
//   seq read:  40.000MB  3.276sec 12.210MB/sec
//   rand read: 40.000MB 151.276sec  0.264MB/sec
// dsites@dclab-2:~/code$   rand read: 40.000MB 151.074sec  0.265MB/sec

// Time on two SSD files simultaniously
// $ ./mystery25 /datassd/dserve/myst25.bin & ./mystery25 /datassd/dserve/myst25a.bin
// [2] 3479
// opening /datassd/dserve/myst25.bin for write
// opening /datassd/dserve/myst25a.bin for write
//   write:     40.000MB  0.010sec 4126.689MB/sec
//   write:     40.000MB  0.016sec 2449.329MB/sec
//   sync:      40.000MB  0.161sec 247.681MB/sec
//   sync:      40.000MB  0.155sec 258.777MB/sec
//   read:      40.000MB  0.109sec 368.636MB/sec
//   read:      40.000MB  0.112sec 356.617MB/sec
//   seq read:  40.000MB  0.944sec 42.363MB/sec
//   seq read:  40.000MB  0.942sec 42.478MB/sec
//   rand read: 40.000MB  0.971sec 41.176MB/sec
//   rand read: 40.000MB  0.971sec 41.176MB/sec

static const int kPageSize = 4096;		// Must be a power of two
static const int kPageSizeMask = kPageSize - 1;

// Make an array bigger than any expected cache size
static const int kMaxArraySize = 40 * 1024 * 1024;


// Order-of-magnitude times:
//  One disk revolution at 7200 RPM = 8.33 msec
//  One disk revolution at 5400 RPM = 11.11 msec 
//  If transfer rate is ~100 MB,sec, a track is ~1MB long, or 256 4KB blocks
//  Time to transfer a single 4KB block is ~40 usec
//  Seek time for big seek is perhaps 15 msec
//  Seek time track-to-track is perhaps 5 msec

// Allocate a byte array of given size, aligned on a page boundary
// Caller will call free(rawptr)
uint8* AllocPageAligned(int bytesize, uint8** rawptr) {
  int newsize = bytesize + kPageSizeMask;
  *rawptr = reinterpret_cast<uint8*>(malloc(newsize));
  uintptr_t temp = reinterpret_cast<uintptr_t>(*rawptr);
  uintptr_t temp2 = (temp + kPageSizeMask) & ~kPageSizeMask;
  return  reinterpret_cast<uint8*>(temp2);
}

// Fill byte array with non-zero pseudo-random bits
void PseudoAll(uint8* ptr, int bytesize) {
  uint32* wordptr = reinterpret_cast<uint32*>(ptr);
  int wordcount = bytesize >> 2;
  uint32 x = POLYINIT32;
  for (int i = 0; i < wordcount; ++i) {
    *wordptr++ = x;
    x = POLYSHIFT32(x);
  }
}

// Set buffer to pseudo-random non-zero bytes
void InitAll(uint8* ptr, int size) {
  kutrace::mark_b("init");
  PseudoAll(ptr, kMaxArraySize);
  kutrace::mark_b("/init");
}

void WriteAll(const char* filename, uint8* ptr, int size) {
  kutrace::mark_a("write");
  fprintf(stdout, "opening %s for write\n", filename);
  int fd = open(filename, O_WRONLY | O_CREAT, S_IRWXU);
  if (fd < 0) {perror("  FAILED write open"); exit(0);}
  FILE* f = fdopen(fd, "wb");
  int64 startusec = GetUsec();
  ssize_t ignoreme = fwrite(ptr, 1, size, f);
  int64 elapsedusec = GetUsec() - startusec;
  fclose(f);
  double mb = size / 1048576.0;
  double sec = elapsedusec / 1000000.0;
  fprintf(stdout, "  write:     %6.2fMB %6.3fsec %6.2f MB/sec\n", mb, sec, mb/sec);
  kutrace::mark_a("/write");
}

void SyncAll(int size) {
  kutrace::mark_b("sync");
  int64 startusec = GetUsec();
  sync();
  int64 elapsedusec = GetUsec() - startusec;
  double mb = size / 1048576.0;
  double sec = elapsedusec / 1000000.0;
  fprintf(stdout, "  sync:      %6.2fMB %6.3fsec %6.2f MB/sec\n", mb, sec, mb/sec);
  kutrace::mark_b("/sync");
}

void ReadAll(const char* filename,uint8* ptr, int size) {
  kutrace::mark_a("read");
  int fd = open(filename, O_RDONLY | O_DIRECT);
  if (fd < 0) {perror("  FAILED read open"); exit(0);}
  FILE* f = fdopen(fd, "rb");
  int64 startusec = GetUsec();
  ssize_t ignoreme = fread(ptr, 1, size, f);
  int64 elapsedusec = GetUsec() - startusec;
  fclose(f);
  double mb = size / 1048576.0;
  double sec = elapsedusec / 1000000.0;
  fprintf(stdout, "  read:      %6.2fMB %6.3fsec %6.2f MB/sec\n", mb, sec, mb/sec);
  kutrace::mark_a("/read");
}

void ReadSeq(const char* filename,uint8* ptr, int size) {
  int blkcount = size >> 12;
  kutrace::mark_b("seq");
  int fd = open(filename, O_RDONLY | O_DIRECT);
  if (fd < 0) {perror("  FAILED read open"); exit(0);}
  FILE* f = fdopen(fd, "rb");
  int64 startusec = GetUsec();
  for (int i = 0; i < blkcount; ++i) {
    ssize_t ignoreme = fread(ptr, 1, 1 <<12, f);
  }
  int64 elapsedusec = GetUsec() - startusec;
  fclose(f);
  double mb = size / 1048576.0;
  double sec = elapsedusec / 1000000.0;
  fprintf(stdout, "  seq read:  %6.2fMB %6.3fsec %6.2f MB/sec\n", mb, sec, mb/sec);
  kutrace::mark_b("/seq");
}

void ReadRand(const char* filename,uint8* ptr, int size) {
  int blkcount = size >> 12;
  uint32 x = POLYINIT32;
  kutrace::mark_a("rand");
  int fd = open(filename, O_RDONLY | O_DIRECT);
  if (fd < 0) {perror("  FAILED read open"); exit(0);}
  FILE* f = fdopen(fd, "rb");
  int64 startusec = GetUsec();
  for (int i = 0; i < blkcount; ++i) {
    int j = x % blkcount;
    x = POLYSHIFT32(x);
    fseek(f, j << 12, SEEK_SET);
    ssize_t ignoreme = fread(ptr, 1, 1 <<12, f);
  }
  int64 elapsedusec = GetUsec() - startusec;
  fclose(f);
  double mb = size / 1048576.0;
  double sec = elapsedusec / 1000000.0;
  fprintf(stdout, "  rand read: %6.2fMB %6.3fsec %6.2f MB/sec\n", mb, sec, mb/sec);
  kutrace::mark_a("/rand");
}

void Usage() {
  fprintf(stderr, "Usage: mystery3 <file name on desired disk>\n");
}

int main (int argc, const char** argv) {
  if (argc < 2) {Usage(); return 0;}
  const char* filename = argv[1];

  kutrace::msleep(100);	// Wait 100 msec so we might start on an idle CPU

  // Allocate a 40MB array aligned on a 4KB boundary
  uint8* rawptr;
  uint8* ptr = AllocPageAligned(kMaxArraySize, &rawptr);

  // Set buffer to pseudo-random non-zero bytes
  InitAll(ptr, kMaxArraySize);
  WriteAll(filename, ptr, kMaxArraySize);
  // Get it really out to disk
  SyncAll(kMaxArraySize);
  ReadAll(filename, ptr, kMaxArraySize);
  ReadSeq(filename, ptr, kMaxArraySize);
  ReadRand(filename, ptr, kMaxArraySize);

  free(rawptr);
  return 0;
}




