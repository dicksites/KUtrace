// Routines to deal with simple locks, using Gnu C intrinsics
// Copyright 2021 Richard L. Sites
//

#ifndef __MUTEX2_H__
#define __MUTEX2_H__

#include "basetypes.h"
#include "fancylock2.h"

// See fancylock2.h
//#define DEFINE_FANCYLOCK2(name, waitus90ile, queue90ile) \
//        FancyLock name(__FILE__, __LINE__, waitus90ile, queue90ile)

// The constructor for this acquires the lock and the destructor releases it.
// Thus, just declaring one of these in a block makes the block run *only* when 
// holding the lock and then reliably release it at block exit
// whoami is any non-negative ID except the largest int 
class Mutex2 {
public:
  Mutex2(int whoami, FancyLock2* flock);	// Acquire lock
  ~Mutex2();					// Release lock

  // Acquire a lock, including a memory barrier to prevent hoisting loads
  // Returns number of usec spent acquiring
  //int32 Acquirelock(FancyLock2::FancyLock2Struct* fstruct);

  // Release a lock, including a memory barrier to prevent sinking stores
  //void Releaselock(FancyLock2::FancyLock2Struct* fstruct);

  // Only data -- Just a pointer to 64-byte fancy lock structure
  FancyLock2* flock_;
};

#endif	// __MUTEX2_H__

