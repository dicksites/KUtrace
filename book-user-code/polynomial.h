// Simple pseudo-random uint8, 32, 64, based on crc32c etc.
// Copyright 2021 Richard L. Sites

#ifndef __POLYNOMIAL_H__
#define __POLYNOMIAL_H__

#include "basetypes.h"

// x should be declared uint8
#define POLY8 (0x1d)   // From CRC-8-SAE J1850
#define POLYSHIFT8(x) ( ((x) << 1) ^ static_cast<uint8>((static_cast<int8>((x)) >> 7) & POLY8) )
#define POLYINIT8 (0xffu)

// x should be declared uint32
#define POLY32 (0x04c11db7)   // From CRC-32
#define POLYSHIFT32(x) ( ((x) << 1) ^ static_cast<uint32>((static_cast<int32>((x)) >> 31) & POLY32) )
#define POLYINIT32 (0xffffffffu)

// x should be declared uint64
#define POLY64 (0x42F0E1EBA9EA3693lu)   // From CRC-64-ECMA
#define POLYSHIFT64(x) ( ((x) << 1) ^ static_cast<uint64>((static_cast<int64>((x)) >> 63) & POLY64) )
#define POLYINIT64 (0xfffffffffffffffflu)

#endif	// __POLYNOMIAL_H__



