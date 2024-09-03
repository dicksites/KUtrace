// Routines to deal with simple mutex lock
// Copyright 2021 Richard L. Sites

#include <stdio.h>
#include <string.h>
#include <unistd.h>		// for syscall
#include <sys/syscall.h>	// for SYS_xxx definitions
#include <linux/futex.h>

#include "basetypes.h"
#include "dclab_log.h"		// for GetUsec()
#include "fancylock2.h"
#include "kutrace_lib.h"
#include "mutex2.h"
#include "timecounters.h"

static const int SPIN_ITER = 8;
static const int SPIN_USEC = 5;

// Global variable. This only has 0->1 transitions after startup and we don't 
// mind losing a few of those, so no threading issues
// 16K bits in this array -- we don't use the two high bits of lnamehash
static uint64 lock_name_added[256] = { 0 };

void TraceLockName(uint16 lnamehash, const char* filename) {
  // We are tracing. Add the name of this lock if not already added
  uint64 bitmask = 1 << (lnamehash & 63);
  int subscr = (lnamehash >> 6) & 255;
  if ((lock_name_added[subscr] & bitmask) != 0) {return;}

  // Remember that we added the name (do this first to mostly avoid 2 threads bothering)
  lock_name_added[subscr] |= bitmask;
  // Add the lock name to the the KUtrace
  uint64 temp[8];
  memset(temp, 0, 8 * sizeof(u64));
  memcpy((char*)(&temp[1]), filename, 22);
  u64 wordlen = 4;
  u64 n_with_length = KUTRACE_LOCKNAME + (wordlen * 16);
  // Build the initial word
  //         T             N                       ARG
  temp[0] = (0l << 44) | (n_with_length << 32) | (lnamehash);
  kutrace::DoControl(KUTRACE_CMD_INSERTN, (u64)&temp[0]);
fprintf(stderr, "Lock name[%04x] %s\n", lnamehash, (const char*)&temp[1]);
}

// Spin a little until lock is available or enough usec pass
// Return true if still locked
bool AcquireSpin(int whoami, int64 start_acquire, FancyLock2::FancyLock2Struct* fstruct) {
  bool old_locked = true;
  do {
    for (int i = 0; i < SPIN_ITER; ++i) {
      if (fstruct->lock == 0) {break;}
      Pause();	// Let any hyperthread in, allow reduced power, slow speculation
    }
    // Lock might be available (0)
    // Try again to get the lock
    old_locked = __atomic_test_and_set(&fstruct->lock, __ATOMIC_ACQUIRE);
    if (!old_locked) {break;}
  } while ((GetUsec() - start_acquire) <= SPIN_USEC);
  return old_locked;
}

// Wait until lock is available 
// Return true if still locked (will always be false on return)
bool AcquireWait(int whoami, int64 start_acquire, FancyLock2::FancyLock2Struct* fstruct) {
  bool old_locked = true;

  // Add us to the number of waiters (not spinners)
  __atomic_add_fetch(&fstruct->waiters, 1, __ATOMIC_RELAXED);

  do {
    // Do futex wait until lock is no longer held (!=1)
    //kutrace::mark_c("wait");
    syscall(SYS_futex, &fstruct->lock, FUTEX_WAIT, 1, NULL, NULL, 0);
    //kutrace::mark_c("/wait");

    // Done futex waiting -- lock is at least temporarily available (0)
    // Try again to get the lock
    old_locked = __atomic_test_and_set(&fstruct->lock, __ATOMIC_ACQUIRE);
  } while (old_locked);

  // Remove us from the number of waiters
  __atomic_sub_fetch(&fstruct->waiters, 1, __ATOMIC_RELAXED);

  return old_locked;
}

//---------------------------------------------------------------------------//
// Exported routines                                                         //
//---------------------------------------------------------------------------//

// Acquire a lock, including a memory barrier to prevent hoisting loads
// fstruct->lock = 0 is available, fstruct->lock = 1 is held by someone else
// whoami is any non-negative ID except the largest int 
// Returns number of usec spent acquiring
int32 Acquirelock(int whoami, FancyLock2* flock) {
  FancyLock2::FancyLock2Struct* fstruct = &flock->fancy2struct_;

  //-----------------------------------//
  // Quick try to get uncontended lock //
  //-----------------------------------//
  bool old_locked = __atomic_test_and_set(&fstruct->lock, __ATOMIC_ACQUIRE);
  if (!old_locked) {
    // Success. We got the lock with no contention
    // Nonetheless there may be waiters outstanding who have not yet retried 
    // Some new waiters may arrive during or after this trace entry but they will
    // generate noacquire entries
    if (0 < fstruct->waiters) {
      // Trace acquire event 
      uint64 words_added = kutrace::addevent(KUTRACE_LOCKACQUIRE, fstruct->lnamehash);
    }

    fstruct->holder = whoami;	// Positive = uncontended
    kutrace::mark_d(0);		// Microseconds to acquire
    return 0;
  }

  //-----------------------------------//
  // Contended lock, we did 1=>1 above //
  //-----------------------------------//
  // Accumulate contended-acquire time 
  int64 start_acquire = GetUsec();
  // Trace contended-lock acquire failed event
  uint64 words_added = kutrace::addevent(KUTRACE_LOCKNOACQUIRE, fstruct->lnamehash);

  //  Add the lock name if tracing and not already added 
  if (words_added == 1) {TraceLockName(fstruct->lnamehash, fstruct->filename);} 

  do {			// Outermost do
    old_locked = AcquireSpin(whoami, start_acquire, fstruct);
    if (!old_locked) {break;}

    old_locked = AcquireWait(whoami, start_acquire, fstruct);
    if (!old_locked) {break;}
  } while (true);	// Outermost do

  // We got the lock
  // Success. We got the lock. Negative indicates contended acquire
  fstruct->holder = ~whoami;	// Bit-complement = contended

  // Trace contended lock-acquire success event
  kutrace::addevent(KUTRACE_LOCKACQUIRE, fstruct->lnamehash);
  // Accumulate contended-acquire time 
  int32 elapsed_acquire = (int32)(GetUsec() - start_acquire);
  //-----------------------------------//
  // End Contended lock                //
  //-----------------------------------//
 
  flock->IncrCounts(elapsed_acquire);
  kutrace::mark_d(elapsed_acquire);	// Microseconds to acquire
  return elapsed_acquire;
}

// Release a lock, including a memory barrier to prevent sinking stores
void Releaselock(FancyLock2* flock) {
  FancyLock2::FancyLock2Struct* fstruct = &flock->fancy2struct_;
  bool was_contended_acquire = (fstruct->holder < 0);
  fstruct->holder = 0x80000000;
  // Do 1=>0
  __atomic_clear(&fstruct->lock, __ATOMIC_RELEASE);

  if (was_contended_acquire || (0 < fstruct->waiters)) {
    // Trace contended-lock free event 
    kutrace::addevent(KUTRACE_LOCKWAKEUP, fstruct->lnamehash);
    // Wake up some (<=4) possible other futex waiters
    //kutrace::mark_b("wake");
    syscall(SYS_futex, &fstruct->lock, FUTEX_WAKE, 4, NULL, NULL, 0);
    //kutrace::mark_b("/wake");
  }
}


// The constructor acquires the lock and the destructor releases it.
// Thus, just declaring one of these in a block makes the block run *only* when 
// holding the lock and then reliably release it at block exit 
// whoami is any non-negative ID except the largest int 
Mutex2::Mutex2(int whoami, FancyLock2* flock) {
  flock_ = flock;
  int32 usec = Acquirelock(whoami, flock_);	// usec not used
}

Mutex2::~Mutex2() {
  Releaselock(flock_);
}

