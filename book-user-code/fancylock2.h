// fancylock2.h 
//
// This defines a software lock that includes some statistics and some identification
//
// Copyright 2021 Richard L. Sites

#ifndef __FANCYLOCK2_H__
#define __FANCYLOCK2_H__

#include "basetypes.h"

#define CACHEALIGNED __attribute__((aligned(64))) 

#define DEFINE_FANCYLOCK2(name, expected_wait_usec) \
        FancyLock2 name(__FILE__, __LINE__, expected_wait_usec)

//
// Fancylock2 (64 bytes, cacheline aligned)
//
//    +-------+-------+-------+-------+-------+-------+-------+-------+
//  0 |             lock              |           waiters             |
//    +-------+-------+-------+-------+-------+-------+-------+-------+
//  8 |                          wait.counts                          |
//    +-------+-------+-------+-------+-------+-------+-------+-------+
// 16 |                          wait.counts_hi                       |
//    +-------+-------+-------+-------+-------+-------+-------+-------+
// 24 | hmin  | hmax  |expec'd| ///// |            holder             |
//    +-------+-------+-------+-------+-------+-------+-------+-------+
// 32 | ///////////////////////////// | ///////////////////////////// |                                                              |
//    +-------+-------+-------+-------+-------+-------+-------+-------+
// 40 |   lnamehash   |           filename
//    +-------+-------+-------+-------+-------+-------+-------+-------+
// 48 |                           filename                            |
//    +-------+-------+-------+-------+-------+-------+-------+-------+
// 56 |                         filename:line                         |
//    +-------+-------+-------+-------+-------+-------+-------+-------+
//

// The constructor initializes a lock variable with declared filename and line#
// The destructor prints contended acquisition time stats
class FancyLock2 {
 public:
  typedef struct {		// Exactly 20 bytes
    uint64 counts;		// This has 8 different power-of-N count buckets bitpacked
    uint64 counts_hi;		// High-order bits of counts, 8 buckets bitpacked
    uint8 hmin;			// minimum log10 value seen, as 3.5 bits
    uint8 hmax;			// maximum log10 value seen, as 3.5 bits
    uint8 expected;		// Expected log10 value, as 3.5 bits
    uint8 pad;			// 
  } CheapHist2;

  // We want this to exactly fill one 64-byte cache line and not be split across two.
  // Filename:linenum is the source file/line where this lock is declared
  typedef struct {
    volatile uint32 lock;	// [0]   0 = unlocked, 1 = locked
    uint32 waiters;		// [4]   0 = no waiters, >0 = N waiters
    CheapHist2 wait;		// [8]
    int32 holder;		// [28] +ID of lock holder if uncontended acquire
 				//      -ID of lockholder if contended acquire
				//      0x80000000 if no holder
    uint32 padding[2];		// [32]
    uint16 lnamehash;		// [40] Hash(filename)
    char filename[22];		// [42] file suffix:linenum plus NUL
  } FancyLock2Struct;

  FancyLock2(const char* filename, const int linenum, 
             const int expected_wait_usec, const int subline = 0);
  ~FancyLock2();

  // Export current 90th percentile acquire time (usec)
  int Get90ile();

  // Record waiting time and queue depth
  void IncrCounts(uint32 wait_us);

  // The only data
  FancyLock2Struct CACHEALIGNED fancy2struct_;
};

void UnpackCounts(uint64 counts, uint32* bucketcounts);


#endif	// __FANCYLOCK2_H__

