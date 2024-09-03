// Routines to deal with simple spinlocks
// Copyright 2021 Richard L. Sites
// Quite possibly flawed

#include "basetypes.h"
#include "dclab_log.h"	// for GetUsec()
#include "spinlock.h"
#include "timecounters.h"

// Global variable. This is constant after startup, so no threading issues
static int kCyclesPerUsec;

// Read the cycle counter and gettimeofday() close together, returning both
void GetTimePair(int64* usec, int64* cycles) {
  uint64 startcy, stopcy;
  int64 gtodusec, elapsedcy;
  // Do more than once if we get an interrupt or other big delay in the middle of the loop
  do {
    startcy = GetCycles();
    gtodusec = GetUsec(); 
    stopcy = GetCycles();
    elapsedcy = stopcy - startcy;
    // In a quick test on an Intel i3 chip, GetUsec() took about 150 cycles
    // printf("%ld elapsed cycles\n", elapsedcy);
  } while (elapsedcy > 10000);	// About 4 usec at 2.5GHz
  *usec = gtodusec;
  *cycles = startcy; 
}

// Loop for 100 ms picking out time of day and cycle counter
// Return measured cycles per usec (expected to be 1000..4000)
int CalibrateCycleCounter() {
  int64 base_usec, base_cycles;
  int64 usec, cycles;
  int64 delta_usec, delta_cycles;
  GetTimePair(&base_usec,&base_cycles);
  do {
    GetTimePair(&usec,&cycles);
    delta_usec = usec - base_usec;
    delta_cycles = cycles - base_cycles;
  } while (delta_usec < 100000);

  kCyclesPerUsec = delta_cycles / delta_usec;  
  return kCyclesPerUsec;
}

// Acquire a spinlock, including a memory barrier to prevent hoisting loads
// Returns number of usec spent spinning
int32 AcquireSpinlock(volatile char* lock) {
  int32 safety_count = 0;
  uint64 startcy = GetCycles(); 
  char old_value;
  do {
    while (*lock != 0) {   // Spin without writing while someone else holds the lock
      ++safety_count;
      if (safety_count > 100000000) {
        fprintf(stderr, "safety_count exceeded. Grabbing lock\n");
        *lock = 0;
      }
    }
    // Try to get the lock
    old_value = __atomic_test_and_set(lock, __ATOMIC_ACQUIRE);
  } while (old_value != 0);
  // WE got the lock
  uint64 stopcy = GetCycles();
  int64 elapsed = stopcy - startcy;
  int32 usec = elapsed / kCyclesPerUsec; 
  return usec;
}

// Release a spinlock, including a memory barrier to prevent sinking stores
void ReleaseSpinlock(volatile char* lock) {
  __atomic_clear(lock, __ATOMIC_RELEASE);
}


// The constructor acquires the spinlock and the destructor releases it.
// Thus, just declaring one of these in a block makes the block run *only* when 
// holding the lock and then reliably release it at block exit 
SpinLock::SpinLock(LockAndHist* lockandhist) {
  lockandhist_ = lockandhist;
  int32 usec = AcquireSpinlock(&lockandhist_->lock);
  ++lockandhist_->hist[FloorLg(usec)];
}

SpinLock::~SpinLock() {
  ReleaseSpinlock(&lockandhist_->lock);
}

