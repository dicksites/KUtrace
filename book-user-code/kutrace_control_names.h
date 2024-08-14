// Names for syscall, etc. in dclab_tracing 
// Copyright 2021 Richard L. Sites
//
// These are from linux-4.19.19 x86 AMD 64-bit. Others will vary.
//

#ifndef __KUTRACE_CONTROL_NAMES_H__
#define __KUTRACE_CONTROL_NAMES_H__

/* Add others as you find and test them */
#define Isx86_64	defined(__x86_64)
#define IsAmd_64	Isx86_64 && defined(__znver1) 
#define IsIntel_64	Isx86_64 && !defined(__znver1)

#define IsArm_64	defined(__aarch64__)
#define IsRPi4		defined(__ARM_ARCH) && (__ARM_ARCH == 8)
#define IsRPi4_64	IsRPi4 && IsArm_64

#if IsAmd_64
#include "kutrace_control_names_ryzen.h"

#elif IsIntel_64
#include "kutrace_control_names_i3.h"

#elif IsRPi4_64
#include "kutrace_control_names_rpi4.h"

#else
#error Need control_names for your architecture
#endif

#endif	// __KUTRACE_CONTROL_NAMES_H__


