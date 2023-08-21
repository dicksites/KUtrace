// Names for syscall, etc. in dclab_tracing
// dick sites 2022.07.31
//

#ifndef __KUTRACE_CONTROL_NAMES_H__
#define __KUTRACE_CONTROL_NAMES_H__

// Add others as you find and test them
#if defined(__linux__)
#define IsLinux         1
#else
#define IsLinux         0
#endif

#if defined(__FreeBSD__)
#define IsFreeBSD         1
#else
#define IsFreeBSD         0
#endif

#if defined(__x86_64__)
#define Isx86_64        1
#else
#define Isx86_64        0
#endif

#if defined(__znver1)
#define IsAmd_64        Isx86_64
#define IsIntel_64	0
#else
#define IsAmd_64        0
#define IsIntel_64	Isx86_64
#endif

#if defined(__aarch64__)
#define IsArm_64        1
#else
#define IsArm_64        0
#endif

#if IsFreeBSD && Isx86_64
#include "kutrace_control_names_freebsd_x86.h"

#elif IsLinux && Isx86_64
#include "kutrace_control_names_linux_x86.h"

#elif IsLinux && IsRiscv_64
#include "kutrace_control_names_linux_riscv.h"

#elif IsLinux && IsArm_64
#include "kutrace_control_names_linux_android.h"

#else
#error Need control_names for your architecture
#endif

#endif	// __KUTRACE_CONTROL_NAMES_H__
