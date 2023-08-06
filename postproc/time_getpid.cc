// Little program to time the KUtrace overhead of the shortest system call
// and of the kutrace::mark_a call. Keep in mind the distortion that can happen
// if run on an idle machine with power-saving slow CPU clocks.
//
// Remember that every call creates TWO KUtrace events, so divide the
// time difference by two after running without, with go, with goipc.

// Copyright 2021 Richard L. Sites

// Compile with g++ -O2 time_getpid.cc kutrace_lib.cc -o time_getpid

// Do 100k getpid() calls
// so we can time these with and without tracing to see the tracing overhead
// dsites 2016.10.13
//
// Intel(R) Celeron(R) CPU G1840 @ 2.80GHz
// no trace 52ns
// go       82ns (15ns per event)
// goipc   126ns (37ns per event, 2.5x more trace overhead)
//
//


// 2018.01.27 After re-mounting heat sink
// Intel(R) Core(TM) i3-7100 CPU @ 3.90GHz
// no trace
// 100000 calls to getpid() took 7992 us (79,72,73,74,74 ns each)	[min 72]
// 100000 calls to mark_a took 3956 us (39,39,39,39,39 ns each)		[min 39]
// trace
// 100000 calls to getpid() took 10162 us (101,100,96,94,96 ns each)	[min 94] +22ns/pair
// 100000 calls to mark_a took 5411 us (54,54,54,54,54 ns each)		[min 54] +15ns/single
// trace with IPC
// 100000 calls to getpid() took 13208 us (132,130,129,134,130 ns each)	[min 129] +57ns/pair
// 100000 calls to mark_a took 7235 us (72,73,73,73,73 ns each)		[min 72]  +33ns/single

// 2020.03.21 baseline RaspberryPi 4B unpatched.These go thru the C runtime library
//  while the above use the inline asm version
// 100000 calls to getpid() took 84965 us (849 ns each) 849, 727, 849, 664, 719, 675, 849, 692, 854
// 100000 calls to mark_a took 39365 us (393 ns each)
// 100000 calls to getpid() took 72724 us (727 ns each)
// 100000 calls to mark_a took 37194 us (371 ns each)
//  difference appears to be related to clockrate after warmup
//  ./time_getpid & ./time_getpid
// 100000 calls to getpid() took 68860 us (688 ns each) 688, 848, 779
// 100000 calls to mark_a took 37415 us (374 ns each)
// 100000 calls to getpid() took 68953 us (689 ns each)
// 100000 calls to mark_a took 39218 us (392 ns each)


#include <sys/types.h> 
#include <unistd.h>

#include <stdio.h>
#include <sys/time.h>	// gettimeofday
#include "basetypes.h"
#include "kutrace_lib.h"

// On ARM-32 /usr/include/arm-linux-gnueabihf/asm/unistd-common.h
// On ARM-64 linux/arch/arm64/include/asm/unistd32.h
// On x86 /usr/include/x86_64-linux-gnu/asm/unistd_64.h

#if defined(__aarch64__)
#define __NR_getpid 172
#elif defined(__ARM_ARCH_ISA_ARM)
#define __NR_getpid 20
#elif defined(__x86_64__)
#define __NR_getpid 39
#elif defined(__riscv)
#define __NR_getpid 172
#else
BUILD_BUG_ON_MSG(1, "Define NR_getpid for your architecture");
#endif


#if 1
// Return current time of day as microseconds since January 1, 1970
inline int64_t GetUsec() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (tv.tv_sec * 1000000l) + tv.tv_usec;
}
#endif


// getpid doesn't actually have any arguments, 
// but I want to compare time to two-arg gettimeofday
inline int64 DoGP(struct timeval* arg1, void* arg2)
{
#if defined(__ARM_ARCH_ISA_ARM) && !defined(__aarch64__)
        register uint32 _arg1 asm("r0") = (uint32)arg1;
        register uint32 _arg2 asm("r1") = (uint32)arg2;
        register uint32 ret asm ("r0");
        register uint32 nr asm("r7") = __NR_getpid;

        asm volatile(
        "       swi #0\n"
        : "=r" (ret)
        : "r" (_arg1), "r" (_arg2), "r" (nr)
        : "memory");

        return ret;

#else 

    int64 retval;
    retval = syscall(__NR_getpid, arg1, arg2);
    return retval;

#endif

// Possible faster version
// #if defined(__x86_64__)
//    asm volatile
//    (
//        "syscall"
//        : "=a" (retval)
//        : "0"(__NR_getpid), "D"(arg1), "S"(arg2)
//        : "cc", "rcx", "r11", "memory"
//    );

}

int main (int argc, const char** argv) {
  int64 bogus = 0;

  // First warm up, to get the CPU clock up to speed
  // No timing here
  for (int i = 0; i < 50000 / 4; ++ i) {
    struct timeval tv; 
    bogus += DoGP(&tv, NULL);
    bogus += DoGP(&tv, NULL);
    bogus += DoGP(&tv, NULL);
    bogus += DoGP(&tv, NULL);
  }
  
  // Now time 100K getpid calls
  int64 start_usec = GetUsec();
  for (int i = 0; i < 100000 / 4; ++ i) {
    struct timeval tv; 
    bogus += DoGP(&tv, NULL);
    bogus += DoGP(&tv, NULL);
    bogus += DoGP(&tv, NULL);
    bogus += DoGP(&tv, NULL);
  }
  int64 stop_usec = GetUsec();

  // Keep bogus as a live variable
  if (stop_usec == 0) {printf("bogus %d\n", (int)bogus);}


  // Now time 100K marker inserts
  int64 start_usec2 = GetUsec();
  for (int i = 0; i < 100000 / 4; ++ i) {
    kutrace::mark_a("hello");
    kutrace::mark_a("hello");
    kutrace::mark_a("hello");
    kutrace::mark_a("hello");
  }
  int64 stop_usec2 = GetUsec();


  // Print last to avoid printing messing up timing
  int delta = stop_usec - start_usec;
  fprintf(stdout, "100000 calls to getpid() took %d us (%d ns each)\n", delta, delta / 100);
  fprintf(stdout, "  Note that each call generates TWO KUtrace events\n");

  int delta2 = stop_usec2 - start_usec2;
  fprintf(stdout, "100000 calls to mark_a took %d us (%d ns each)\n", delta2, delta2 / 100);

  return 0;
}
