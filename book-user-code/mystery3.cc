// Little program to time disk transfers
// Copyright 2021 Richard L. Sites

// Usage: mystery3 <file name on desired disk>
// Compile with 
//   g++ -O2 mystery3.cc -lrt -o mystery3_opt
// The -lrt flag option is required to use async i/o
// Using g++ instead of gcc because we are using C++ strings

#include <aio.h>		// Async I/O. MUST LINK WITH -lrt
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>		// gettimeofday
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <string>

#include "basetypes.h"
#include "polynomial.h"
#include "timecounters.h"

using std::string;

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

// Zero a byte array
void ZeroAll(uint8* ptr, int bytesize) {
  memset(ptr, 0, bytesize);
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

// Map all usec times into single character
// 0..100 usec to a..j
// 100..1000 usec to 1..9
// 1000+ to A..Z
char OneChar(int64 usec) {
  if (usec < 0) {return '-';}
  if (usec < 10) {return '.';}
  if (usec < 100) {return (usec / 10) + 'a';}
  if (usec < 1000) {return (usec / 100) + '0';}
  if (usec < 25000) {return (usec / 1000) + 'A';}
  return '+';
}

void PrintLegend(FILE* f, const char* label) {
  // ASCII art delta-times
  fprintf(f, "%s: 4KB block delta times in usec\n", label);
  fprintf(f, "  - negative delta-time\n");
  fprintf(f, "  . <10us delta-time\n");
  fprintf(f, "  b<20 c<30 d<40 e<50 f<60 g<70 h<80 i<90 j<100us\n");
  fprintf(f, "  1<200 2<300 3<400 4<500 5<600 6<700 7<800 8<900 9<1000us\n");
  fprintf(f, "  B<2 C<3 D<4 E<5 F<6 G<7 H<8 I<9 J<10.. Y<25ms\n");
  fprintf(f, "  + >=25ms delta-time\n");
  fprintf(f, "\n");
}


// Print out the delta times in usec
void PrintTimes(const char* fname, const char* label, const int64* usecperblock, int numblocks) {
  FILE* f = fopen(fname, "w");
  if (f == NULL) {return;}
  // Print ASCII art of the delta times
  PrintLegend(f, label);
  int runcount = 0;
  int64 runtime = 0;
  int64 currenttime = 0;
  for (int i = 0; i < numblocks; ++i) {
    if ((i & 255) == 255) {fprintf(f, " [%dMB]", (i + 1) / 256);}
    if ((i & 63) == 63) {fprintf(f, "\n");}

    // Negative or large times (greater than 1000 usec) finish the old run
    if ((usecperblock[i] < 0) || (usecperblock[i] > 1000)) {
      if (runtime > 0) {
        fprintf(f, "\n  = %d blocks %5.3fms %4.2fMB/s\n", 
                runcount, runtime / 1000.0, (runcount * 4096.0) / runtime);
      }
    }

    currenttime += usecperblock[i];

    // Negative or large times start a new run, do not contribute to total time
    if ((usecperblock[i] < 0) || (usecperblock[i] > 1000)) {
      runcount = 0;
      runtime = 0;
      fprintf(f, "(%+5.3fms) @ %5.3fms\n", usecperblock[i] / 1000.0, currenttime / 1000.0);
    } else {
      runtime += usecperblock[i];
    }

    fprintf(f, "%c", OneChar(usecperblock[i]));
    ++runcount;
  }

  if (runtime > 0) {
    fprintf(f, "\n  = %d blocks %5.3fms %4.2fMB/s\n", 
            runcount, runtime / 1000.0, (runcount * 4096.0) / runtime);
  }
  fprintf(f, "\n");

  // Print raw delta-times
  for (int i = 0; i < numblocks; ++i) {
    fprintf(f, "%3lld ", usecperblock[i]);
    if ((i & 255) == 255) {fprintf(f, " [%dMB]", (i + 1) / 256);}
    if ((i & 15) == 15) {fprintf(f, "\n");}
  }
  fclose(f);
}

// Print out the delta times in usec, suitable for JavaScript import
void PrintJSON(const char* fname, const char* label, const int64* usecperblock, int numblocks) {
  FILE* f = fopen(fname, "w");
  if (f == NULL) {return;}

  bool reading = (strstr(label, "ead") != NULL);

  fprintf(f, "   {\n");
  fprintf(f, "  \"axisLabelX\" : \"Time (sec)\",\n");
  fprintf(f, "  \"axisLabelY\" : \"Offset in file (blocks)\",\n");
  fprintf(f, "  \"dotColor\" : \"%s\",\n", reading ? "blue" : "red");
  fprintf(f, "  \"dotR\" : 3,\n");
  fprintf(f, "  \"shortUnitsX\" : \"s\",\n");
  fprintf(f, "  \"shortUnitsY\" : \"B\",\n");
  fprintf(f, "  \"shortMulX\" : 1,\n");
  fprintf(f, "  \"shortMulY\" : 4096,\n");
  fprintf(f, "  \"thousandsX\" : 1000,\n");
  fprintf(f, "  \"thousandsY\" : 1024,\n");
  fprintf(f, "  \"title\" : \"Disk/SSD %s 4KB blocks vs. time\",\n", label);
  fprintf(f, " \"points\" : [\n");

  // Raw current times (w.r.t. start time)
  int64 currenttime = 0;
  for (int i = 0; i < numblocks; ++i) {
    currenttime += usecperblock[i];
    fprintf(f, "[%8.6f, %5d],\n", currenttime / 1000000.0, i);
  }
  fprintf(f, "[999.000000, 0]\n");	// End marker; No comma
  fprintf(f, "]}\n");

  fclose(f);
}


// Timing disk reads
// Strategy: 
//   Write a file of pseudo-random data that is bigger than any expected 
//     on-disk track buffer.
//   Do an asynchronous read into an all-zero buffer. 
//   While that is happening, scan the beginning of each 4KB block in the 
//     buffer looking for a change from zero to non-zero. 
//   Record the time at each change.
//   After the read completes, return the delta-time for each block.
void TimeDiskRead(uint8* ptr, int bytesize, const char* filename, 
                  int64* usecperblock, int numblocks) {
  // Zero the array of block times
  memset(usecperblock, 0, numblocks * sizeof(int64));

  // Set buffer to pseudo-random non-zero bytes
  PseudoAll(ptr, bytesize);

  // Open target file 
  fprintf(stdout, "TimeDiskRead opening %s for write\n", filename);
  int fd = open(filename, O_WRONLY | O_CREAT, S_IRWXU);
  if (fd < 0) {perror("TimeDiskRead write open"); return;}

  // Write it
  ssize_t byteswritten = write(fd, ptr, bytesize);
  if (byteswritten < 0) {perror("TimeDiskRead write"); return;}
  close(fd);

  // Set buffer to zero bytes
  ZeroAll(ptr, bytesize);

  // Prepare for asynchronous read
  fprintf(stdout, "TimeDiskRead opening %s for read of %dKB\n", filename, numblocks * 4);
  fd = open(filename, O_RDONLY | O_DIRECT | O_NOATIME | O_ASYNC);
  if (fd < 0) {perror("TimeDiskRead read open"); return;}

  struct aiocb aiocbp;
  memset(&aiocbp, 0, sizeof(aiocb));
  aiocbp.aio_fildes = fd;
  aiocbp.aio_offset = 0;
  aiocbp.aio_buf = ptr;
  aiocbp.aio_nbytes = bytesize;
  aiocbp.aio_reqprio = 0;
  aiocbp.aio_sigevent.sigev_notify = SIGEV_NONE;
  //aiocbp.aio_lio_opcode = LIO_NOP;
      
  // It is quite possible at this point that the write to disk is still happening.
  // In that case, the lowest time we see after startusec might be much larger than
  // one seek time. Doing a sync here helps separate the write time from the 
  // upcoming read time
  syncfs(fd);

  // Start timer and the read
  int64 startusec, stopusec;
  startusec = GetUsec();

  int ok = aio_read(&aiocbp);
  if (ok < 0) {perror("TimeDiskRead aio_read"); return;}
  
  // Async read is now happening

  // Scanning the buffer for non-zero values may take longer than the time to read
  // a single disk block. Scanning is good because it discovers blocks arriving
  // in an arbitrary unexpected order. But to give better time resolution, we
  // look every time at the next-sequential block also. 
  int expected_i = 0;		// The 4KB block number we expect to be read next
  uint8* expected_ptr4kb = ptr;  // Its first word
  int scancount = 0;
  int changecount = 0;
  int ptr4kb_incr = kPageSize / sizeof(uint32);
  while(aio_error(&aiocbp) == EINPROGRESS) {
    // Read is still going on
    uint8* ptr4kb = ptr;
    int64 timeusec = GetUsec();
    for (int i = 0; i < numblocks; ++i) {
      if ((i & 255) == 0) {timeusec = GetUsec();}
      // Scan for new non-zero values
      if ((usecperblock[expected_i] == 0) && 
          (*reinterpret_cast<int64*>(expected_ptr4kb) != 0)) {
        // We just saw a change
        usecperblock[expected_i] = timeusec;
        ++changecount;
        expected_i = expected_i + 1;	// Expect next sequential block
        expected_ptr4kb = expected_ptr4kb + kPageSize;
      }
      if ((usecperblock[i] == 0) && 
          (*reinterpret_cast<int64*>(ptr4kb) != 0)) {
        // We just saw a change
        usecperblock[i] = timeusec;
        ++changecount;
        expected_i = i + 1;		// Expect next sequential block
        expected_ptr4kb = ptr4kb + kPageSize;
      }
      ptr4kb += kPageSize;	// Next 4KB block 
    }
    ++scancount;
  }
  // Async read is now complete
  stopusec = GetUsec();
  double felapsedusec = stopusec - startusec;

  // Fill in any missed times
  for (int i = 0; i < numblocks; ++i) {
    if (usecperblock[i] == 0) {usecperblock[i] = stopusec;}
  }


  fprintf(stdout, "Async read startusec %lld, stopusec  %lld, delta %lld\n", 
          startusec, stopusec, stopusec - startusec);

  fprintf(stdout, "scancount %d, changecount inside scan %d\n", 
          scancount, changecount);
  fprintf(stdout, "  %5.3fMB/sec overall\n\n", 
          bytesize / felapsedusec);

  ssize_t bytesread = aio_return(&aiocbp);
  if (bytesread < 0) {perror("TimeDiskRead aio_read"); return;}
  close(fd);

  // Put delta times into return array
  int64 priorchangetime = startusec;
  for (int i = 0; i < numblocks; ++i) {
    int64 temp = usecperblock[i];
    usecperblock[i] = usecperblock[i] - priorchangetime;    // This can be negative!
    priorchangetime = temp;
  }
}


// Timing disk writes
// Strategy: 
//   Write a buffer of pseudo-random data that is bigger than any expected 
//     on-disk track buffer.
//   Do an asynchronous write to disk. 
//   While that is happening, scan the beginning of each 4KB block in the 
//     buffer writing the time.
//   After the write completes, read back the data to see what times got to disk.
//   Return the delta-time for each block.
void TimeDiskWrite(uint8* ptr, int bytesize, const char* filename, 
                  int64* usecperblock, int numblocks) {
  fprintf(stderr, "TimeDiskWrite to be completed\n");
  
  // Zero the array of block times
  memset(usecperblock, 0, numblocks * sizeof(int64));

  // Set buffer to pseudo-random non-zero bytes
  PseudoAll(ptr, bytesize);

  // Set the times at the front of each 4KB block all to zero
  uint8* ptr4kb = ptr;    // Its first 4KB block
  for (int i = 0; i < numblocks; ++i) {
    *reinterpret_cast<int64*>(ptr4kb) = 0;
    ptr4kb += kPageSize;
  }

  // Prepare for asynchronous write
  fprintf(stdout, "TimeDiskWrite  opening %s for async write of %dKB\n", 
          filename, numblocks * 4);
  int fd = open(filename, O_WRONLY | O_CREAT | O_DIRECT | O_NOATIME | O_ASYNC, S_IRWXU);
  if (fd < 0) {perror("TimeDiskWrite write open"); return;}

  struct aiocb aiocbp;
  memset(&aiocbp, 0, sizeof(aiocb));
  aiocbp.aio_fildes = fd;
  aiocbp.aio_offset = 0;
  aiocbp.aio_buf = ptr;
  aiocbp.aio_nbytes = bytesize;
  aiocbp.aio_reqprio = 0;
  aiocbp.aio_sigevent.sigev_notify = SIGEV_NONE;
  //aiocbp.aio_lio_opcode = LIO_NOP;
      
  // It is quite possible at this point that the open is still happening.
  // In that case, the lowest time we see after startusec might be much larger than
  // one seek time. Doing a sync here helps separate the open time from the 
  // upcoming write time
  syncfs(fd);

  // Start timer and the write
  int64 startusec, stopusec;
  startusec = GetUsec();

  int ok = aio_write(&aiocbp);
  if (ok < 0) {perror("TimeDiskWrite aio_write"); return;}
  
  // Async write is now happening

  while(aio_error(&aiocbp) == EINPROGRESS) {
    // Write is still going on
    // Repeatedlly put current time into the front of each 4KB block
    // (No shortcuts available to get better time resolution)

////
//// You get to fill in this part !!
////

  }
  // Async write is now complete
  stopusec = GetUsec();
  double felapsedusec = stopusec - startusec;

  fprintf(stdout, "Async write startusec %lld, stopusec  %lld, delta %lld\n", 
          startusec, stopusec, stopusec - startusec);
  fprintf(stdout, "  %5.3fMB/sec overall\n\n", 
          bytesize / felapsedusec);

  ssize_t byteswritten = aio_return(&aiocbp);
  if (byteswritten < 0) {perror("TimeDiskWrite aio_write"); return;}
  close(fd);

  // Now read back the file and see what times went out
  // Open target file 
  fprintf(stdout, "TimeDiskWrite opening %s for read\n", filename);
  fd = open(filename, O_RDONLY);
  if (fd < 0) {perror("TimeDiskWrite read open"); return;}

  // Zero the buffer for cleanliness in case anything went wrong
  ZeroAll(ptr, bytesize);

  // Read it
  ssize_t bytesread = read(fd, ptr, bytesize);
  if (bytesread < 0) {perror("TimeDiskWrite read"); return;}
  close(fd);

  // Extract raw times from front of each 4KB block and put in return array
  ptr4kb = ptr;    // Its first 4KB block
  for (int i = 0; i < numblocks; ++i) {
    usecperblock[i] = *reinterpret_cast<int64*>(ptr4kb);
    ptr4kb += kPageSize;
  }
  // Fill in any missed times
  for (int i = 0; i < numblocks; ++i) {
    if (usecperblock[i] == 0) {usecperblock[i] = startusec;}
  }
 
  // Put delta times into return array
  int64 priorchangetime = startusec;
  for (int i = 0; i < numblocks; ++i) {
    int64 temp = usecperblock[i];
    usecperblock[i] = usecperblock[i] - priorchangetime;    // This can be negative!
    priorchangetime = temp;
  }
}

string StripSuffix(const char* fname) {
  string str = string(fname);
  size_t period = str.find_last_of('.');
  if (period == string::npos) {return str;}
  return str.substr(0, period); 
}


void Usage() {
  fprintf(stderr, "Usage: mystery3 <file name on desired disk>\n");
}

int main (int argc, const char** argv) {
  if (argc < 2) {Usage(); return 0;}

  const char* filename = argv[1];

  // Allocate a 40MB array aligned on a 4KB boundary
  uint8* rawptr;
  uint8* ptr = AllocPageAligned(kMaxArraySize, &rawptr);

  // Allocate usec counts per 4KB disk block. Signed to allow negative deltas
  int numblocks = kMaxArraySize / kPageSize;
  int64* usecperblock = new int64[numblocks];
  memset(usecperblock, 0, numblocks * sizeof(int64));

  TimeDiskRead(ptr, kMaxArraySize, filename, usecperblock, numblocks);
  string rtime_fname = StripSuffix(filename) + "_read_times.txt";
  string rjson_fname = StripSuffix(filename) + "_read_times.json";
  PrintTimes(rtime_fname.c_str(), "Read", usecperblock, numblocks);
  PrintJSON(rjson_fname.c_str(), "Read", usecperblock, numblocks);
  fprintf(stderr, "%s and %s written\n", rtime_fname.c_str(), rjson_fname.c_str());

  TimeDiskWrite(ptr, kMaxArraySize, filename, usecperblock, numblocks);
  string wtime_fname = StripSuffix(filename) + "_write_times.txt";
  string wjson_fname = StripSuffix(filename) + "_write_times.json";
  fprintf(stderr, "%s and %s written\n", wtime_fname.c_str(), wjson_fname.c_str());
  PrintTimes(wtime_fname.c_str(), "Write", usecperblock, numblocks);
  PrintJSON(wjson_fname.c_str(), "Write", usecperblock, numblocks);

  delete[] usecperblock;
  free(rawptr);
  return 0;
}




