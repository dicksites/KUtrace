// timercounters.h 
//
// Reading cycle counter and gettimeofday counter, on various architectures 
// Also Pause() to slow down speculation in spin loops and give cycles to any hyperthread
//
// Copyright 2022 Richard L. Sites


#ifndef __TIMERCOUNTERS_H__
#define __TIMERCOUNTERS_H__

/* Add others as you find and test them */
#define Isx86_64	defined(__x86_64)
#define IsAmd_64	Isx86_64 && defined(__znver1) 
#define IsIntel_64	Isx86_64 && !defined(__znver1)

#define IsArm_64	defined(__aarch64__)
#define IsRPi4		defined(__ARM_ARCH) && (__ARM_ARCH == 8)
#define IsRPi4_64	IsRPi4 && IsArm_64
#define IsRiscv		defined(__riscv)

#include <sys/time.h>		// gettimeofday

#if Isx86_64
#include <x86intrin.h>		// __rdtsc()
#endif

// Return a constant-rate "cycle" counter
inline int64_t GetCycles() {
#if Isx86_64
   // Increments once per cycle, implemented as increment by N every N (~35) cycles
   return __rdtsc();

#elif IsRPi4_64
  // Increments once per 27.778 cycles for RPi4-B at 54 MHz counter and 1.5GHz CPU clock
  // Call it 28 cycles
  uint64_t counter_value;
  asm volatile("mrs %x0, cntvct_el0" : "=r"(counter_value));
  return counter_value * 28;

#elif IsRiscv
  // Increments at unknown rate, might be 1 usec
  
#define CSR_CYCLE               0xc00
#define CSR_TIME                0xc01
#define CSR_INSTRET             0xc02

#ifdef __ASSEMBLY__
#define __ASM_STR(x)    x
#else
#define __ASM_STR(x)    #x
#endif

#define csr_read(csr)                                           \
({                                                              \
        unsigned long __v;                             \
        __asm__ __volatile__ ("csrr %0, " __ASM_STR(csr)        \
                              : "=r" (__v) :                    \
                              : "memory");                      \
        __v;                                                    \
})

  return csr_read(CSR_TIME);

#else
#error Need cycle counter defines for your architecture
#endif
 
}

// Return current time of day as microseconds since January 1, 1970
inline int64_t GetUsec() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (tv.tv_sec * 1000000l) + tv.tv_usec;
}

inline void Pause() {
#if Isx86_64
  __pause();
#else
  // Nothing on Arm, etc. 
#endif
}


#endif	// __TIMERCOUNTERS_H__
