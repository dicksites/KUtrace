// Names for syscall, etc. in dclab_tracing 
// dick sites 2022.07.31
//

#ifndef __KUTRACE_CONTROL_NAMES_H__
#define __KUTRACE_CONTROL_NAMES_H__

// Add others as you find and test them 
#define IsFreeBSD	defined(__FreeBSD__)
#define IsLinux		defined(__linux__)

#define Isx86_64	defined(__x86_64)
#define IsAmd_64	defined(Isx86_64) && defined(__znver1) 
#define IsIntel_64	defined(Isx86_64) && !defined(__znver1)
#define IsArm_64	defined(__aarch64__)
#define IsRPi4		defined(__ARM_ARCH) && (__ARM_ARCH == 8)
#define IsRPi4_64	defined(IsRPi4) && defined(IsArm_64)

#if IsFreeBSD && Isx86_64
#include "kutrace_control_names_freebsd_x86.h"

#elif IsLinux && Isx86_64
#include "kutrace_control_names_linux_x86.h"

#elif IsLinux && IsRiscv_64
#include "kutrace_control_names_linux_riscv.h"

#elif IsLinux && IsRPi4_64
#include "kutrace_control_names_linux_rpi4.h"

#else
#error Need control_names for your architecture
#endif

#endif	// __KUTRACE_CONTROL_NAMES_H__
