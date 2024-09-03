// Routines to deal with simple spinlocks, using Gnu C intrinsics
// Copyright 2021 Richard L. Sites

#ifndef __SPINLOCK_H__
#define __SPINLOCK_H__

#include "basetypes.h"

typedef struct {
  volatile char lock;	// One-byte spinlock
  char pad[7];		// align the histogram
  uint32 hist[32];	// histogram of spin time, in buckets of floor(lg(usec))
} LockAndHist;

// The constructor for this acquires the spinlock and the destructor releases it.
// Thus, just declaring one of these in a block makes the block run *only* when 
// holding the lock and then reliably release it at block exit 
class SpinLock {
public:
  SpinLock(LockAndHist* lockandhist);
  ~SpinLock();

  LockAndHist* lockandhist_;
};

// Return floor log 2 of x, i.e. the number of bits needed to hold x
int32 FloorLg(int32 x);

// Read the cycle counter and gettimeofday() close together, returning both
void GetTimePair(int64* usec, int64* cycles);

// Loop for 100 ms picking out time of day and cycle counter
// Return measured cycles per usec (expected to be 1000..4000)
// Sets an internal global variable for AcquireSpinlock
int CalibrateCycleCounter();

// Acquire a spinlock, including a memory barrier to prevent hoisting loads
// Returns number of usec spent spinning
int32 AcquireSpinlock(volatile char* lock);

// Release a spinlock, including a memory barrier to prevent sinking stores
void ReleaseSpinlock(volatile char* lock);

#endif	// __SPINLOCK_H__

