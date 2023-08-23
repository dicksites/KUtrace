// Base types to use thoughout class
// dick sites 2020.03.31

#ifndef __BASETYPES_H__
#define __BASETYPES_H__

#include <stdint.h>

typedef int8_t int8;
typedef uint8_t uint8;
typedef int16_t int16;
typedef uint16_t uint16;
typedef int32_t int32;
typedef uint32_t uint32;

#ifdef __ARM_ARCH_ISA_ARM

typedef long long int int64;
typedef long long unsigned int uint64;
#define FUINTPTRX "%08lx"
#define FLX "%016llx"
#define FLD "%lld"
#define CL(x) x##LL
#define CLU(x) x##LLU

#elif defined(__aarch64__)
typedef long long int int64;
typedef long long unsigned int uint64;
#define FUINTPTRX "%016lx"
#define FLX "%016llx"
#define FLD "%lld"
#define CL(x) x##LL
#define CLU(x) x##LLU

#elif defined(__x86_64)
/* make almost the same as ARM-32 */
typedef long long int int64;
typedef long long unsigned int uint64;
#define FUINTPTRX "%016lx"
#define FLX "%016llx"
#define FLD "%lld"
#define CL(x) x##LL
#define CLU(x) x##LLU

#elif 0
/* actual 64-bit types */
typedef long int int64;
typedef long unsigned int uint64;
#define FUINTPTRX "%016lx"
#define FLX "%016lx"
#define FLD "%ld"
#define CL(x) x##L
#define CLU(x) x##LU

#else
#error Need type defines for your architecture
#endif

#endif	// __BASETYPES_H__
