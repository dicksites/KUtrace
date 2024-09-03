// Matrix multiply experiments, looking at cache blocking
// Copyright 2021 Richard L. Sites

#include <stdio.h>
#include <string.h>
#include <sys/time.h>	// gettimeofday
#include "basetypes.h"
#include "kutrace_lib.h"
#include "timecounters.h"

// compile with g++ -O2 matrix.cc  kutrace_lib.cc  -o matrix_ku 


//
// Sample timings, with cache simulation and miss counts
//
// SimpleMultiply            	87.137 seconds, sum=2494884076.030955315
// Misses L1/L2/L3 1077341184 1058414205  886641817
//
// remapsize = 8
// BlockMultiplyRemap        	38.692 seconds, sum=2494884076.030973911
// Misses L1/L2/L3   35197695   17894256   15430436
//
// remapsize = 16
// BlockMultiplyRemap        	37.028 seconds, sum=2494884076.030989647
// Misses L1/L2/L3   19917433   10416455    8750623
//
// remapsize = 32
// BlockMultiplyRemap        	37.840 seconds, sum=2494884076.030955315
// Misses L1/L2/L3   26161141    8116254    5228737
//
// remapsize = 64
// BlockMultiplyRemap        	46.340 seconds, sum=2494884076.030960083
// Misses L1/L2/L3  111357948    6005061    3741151
//
// SimpleMultiplyTranspose   	57.594 seconds, sum=2494884076.030954838
// Misses L1/L2/L3  268100748  147229568  141132672
//
//
// Without cache simulation
// SimpleMultiply            	6.478 seconds, sum=2494884076.030955315
// Misses L1/L2/L3          0          0          0
// remapsize = 8
// BlockMultiplyRemap        	0.598 seconds, sum=2494884076.030973911
// Misses L1/L2/L3          0          0          0
// remapsize = 16
// BlockMultiplyRemap        	0.419 seconds, sum=2494884076.030989647
// Misses L1/L2/L3          0          0          0
// remapsize = 32
// BlockMultiplyRemap        	0.368 seconds, sum=2494884076.030955315
// Misses L1/L2/L3          0          0          0
// remapsize = 64
// BlockMultiplyRemap        	0.356 seconds, sum=2494884076.030960083
// Misses L1/L2/L3          0          0          0
//
// SimpleMultiplyTranspose   	1.139 seconds, sum=2494884076.030955315
// Misses L1/L2/L3          0          0          0
//
// Without low bits L3 set selection
// SimpleMultiply            	87.900 seconds, sum=2494884076.030955315
// Misses L1/L2/L3 1077341184 1058414205  886641817
// SimpleMultiplyColumnwise  	80.283 seconds, sum=2494884076.030955315
// Misses L1/L2/L3 1209008128 1209008128 1092145348
//
// With XOR in L3 set selection
// SimpleMultiply            	90.130 seconds, sum=2494884076.030955315
// Misses L1/L2/L3 1077341184 1058414205  746808422
// SimpleMultiplyColumnwise  	88.247 seconds, sum=2494884076.030955315
// Misses L1/L2/L3 1209008128 1209008128  185272771
//                                        --------- net 20% faster than rowwise
// 00
// SimpleMultiply            	6.482 seconds, sum=2494884076.030955315
// Misses L1/L2/L3          0          0          0
// SimpleMultiplyColumnwise  	5.115 seconds, sum=2494884076.030955315
// Misses L1/L2/L3          0          0          0
// SimpleMultiplyTranspose   	0.584 seconds, sum=2494884076.030954838
// Misses L1/L2/L3          0          0          0
// BlockMultiplyRemap        	0.602 seconds, sum=2494884076.030973911
// Misses L1/L2/L3          0          0          0
//
// 01
// SimpleMultiply            	6.458 seconds, sum=2494884076.030955315
// Misses L1/L2/L3          0          0          0
// SimpleMultiplyColumnwise  	5.211 seconds, sum=2494884076.030955315
// Misses L1/L2/L3          0          0          0
// SimpleMultiplyTranspose   	0.594 seconds, sum=2494884076.030954838
// Misses L1/L2/L3          0          0          0
// BlockMultiplyRemap        	0.630 seconds, sum=2494884076.030973911
// Misses L1/L2/L3          0          0          0
//
// 10
// Remap Misses L1/L2/L3     524288     523928     522560
// Transpose Misses L1/L2/L3    2359296    2359258    2342724
// SimpleMultiply            	87.108 seconds, sum=2494884076.030955315
// Misses L1/L2/L3 1077341184 1058414205  886641817
// SimpleMultiplyColumnwise  	79.103 seconds, sum=2494884076.030955315
// Misses L1/L2/L3 1209008128 1209008128 1092145348
// SimpleMultiplyTranspose   	57.489 seconds, sum=2494884076.030954838
// Misses L1/L2/L3  268100748  147229568  141132672
// BlockMultiplyRemap        	38.472 seconds, sum=2494884076.030973911
// Misses L1/L2/L3   35197695   17894256   15430436
//
// 11
// Remap Misses L1/L2/L3     524288     523928     518315
// Transpose Misses L1/L2/L3    2359296    2359258     943198
// SimpleMultiply            	89.591 seconds, sum=2494884076.030955315
// Misses L1/L2/L3 1077341184 1058414205  751193415
// SimpleMultiplyColumnwise  	87.377 seconds, sum=2494884076.030955315
// Misses L1/L2/L3 1209008128 1209008128  184542843
// SimpleMultiplyTranspose   	64.925 seconds, sum=2494884076.030954838
// Misses L1/L2/L3  268100748  147229568  132833587
// BlockMultiplyRemap        	42.061 seconds, sum=2494884076.030973911
// Misses L1/L2/L3   35197695   17894256   16794674
//
// 00 w/slow transpose
// SimpleMultiply            	6.422 seconds, sum=2494884076.030955315
// Misses L1/L2/L3          0          0          0
// SimpleMultiplyColumnwise  	5.162 seconds, sum=2494884076.030955315
// Misses L1/L2/L3          0          0          0
// SimpleMultiplyTranspose   	1.138 seconds, sum=2494884076.030955315
// Misses L1/L2/L3          0          0          0
// SimpleMultiplyTransposeFast	0.586 seconds, sum=2494884076.030954838
// Misses L1/L2/L3          0          0          0
// BlockMultiplyRemap        	0.613 seconds, sum=2494884076.030973911
// Misses L1/L2/L3          0          0          0
//
// 01 w/slow transpose
// SimpleMultiply            	6.365 seconds, sum=2494884076.030955315
// Misses L1/L2/L3          0          0          0
// SimpleMultiplyColumnwise  	5.044 seconds, sum=2494884076.030955315
// Misses L1/L2/L3          0          0          0
// SimpleMultiplyTranspose   	1.144 seconds, sum=2494884076.030955315
// Misses L1/L2/L3          0          0          0
// SimpleMultiplyTransposeFast	0.579 seconds, sum=2494884076.030954838
// Misses L1/L2/L3          0          0          0
// BlockMultiplyRemap        	0.601 seconds, sum=2494884076.030973911
// Misses L1/L2/L3          0          0          0
//
// 10 w/slow transpose
// Transpose Misses L1/L2/L3    2359296    2359258    2342724
// BlockTranspose Misses L1/L2/L3     552960     524395     522240
//
// SimpleMultiply            	87.978 seconds, sum=2494884076.030955315
// Misses L1/L2/L3 1077341184 1058414205  886641817
// SimpleMultiplyColumnwise  	79.212 seconds, sum=2494884076.030955315
// Misses L1/L2/L3 1209008128 1209008128 1092145348
// SimpleMultiplyTranspose   	43.685 seconds, sum=2494884076.030955315
// Misses L1/L2/L3  269018803  148146944  142050176
// SimpleMultiplyTransposeFast	58.290 seconds, sum=2494884076.030954838
// Misses L1/L2/L3  268100748  147229568  141132672
// BlockMultiplyRemap        	39.473 seconds, sum=2494884076.030973911
// Misses L1/L2/L3   35197695   17894256   15430436
//
// 11 w/slow transpose
// Transpose Misses L1/L2/L3    2359296    2359258    1019221
// BlockTranspose Misses L1/L2/L3     552960     524395     522427
//
// SimpleMultiply            	90.130 seconds, sum=2494884076.030955315
// Misses L1/L2/L3 1077341184 1058414205  752975084
// SimpleMultiplyColumnwise  	89.653 seconds, sum=2494884076.030955315
// Misses L1/L2/L3 1209008128 1209008128  183260146
// SimpleMultiplyTranspose   	44.817 seconds, sum=2494884076.030955315
// Misses L1/L2/L3  269018803  148146944  133124904
// SimpleMultiplyTransposeFast	61.296 seconds, sum=2494884076.030954838
// Misses L1/L2/L3  268100748  147229568  132811796
// BlockMultiplyRemap        	38.003 seconds, sum=2494884076.030973911
// Misses L1/L2/L3   35197695   17894256   16808337
// 
// 32x32 remap
// BlockMultiplyRemap        	0.373 seconds, sum=2494884076.030955315
// Misses L1/L2/L3   26161141    8116254    5228737
//
// BlockMultiplyRemap        	0.392 seconds, sum=2494884076.030955315
// Misses L1/L2/L3   26161141    8116254    5243627
//
// Remap Misses L1/L2/L3     524288     524288     523376
// Remap Misses L1/L2/L3     524288     524288     517579
// 

#define TRACK_CACHES 0
#define HASHED_L3 0

static const int kRowsize = 1024;
static const int kColsize = kRowsize;
static const int kBlocksize = 8;

static const int kRemapsize = 32;
//static const int kRemapsize = 16;
//static const int kRemapsize = 32;
//static const int kRemapsize = 64;

////typedef unsigned long int uint64;
typedef void MulProc(const double* a, const double* b, double* c);

static double* aa = NULL;
static double* bb = NULL;
static double* cc = NULL;

static const int kL1LgSize = 15;
static const int kL1LgAssoc = 3;
static const int kL1LgLinesize = 6;
static const int kL1LgSetsize = kL1LgSize - kL1LgAssoc - kL1LgLinesize;
static const int kL1Setsize = 1 << kL1LgSetsize;
static const int kL1Assoc = 1 << kL1LgAssoc;
static const int kL1Assocmask = kL1Assoc - 1;
static const uint64 kL1Setmask = (1l << kL1LgSetsize) - 1;
static const uint64 kL1Tagmask = (1l << kL1LgLinesize) - 1;

static const int kL2LgSize = 18;
static const int kL2LgAssoc = 3;
static const int kL2LgLinesize = 6;
static const int kL2LgSetsize = kL2LgSize - kL2LgAssoc - kL2LgLinesize;
static const int kL2Setsize = 1 << kL2LgSetsize;
static const int kL2Assoc = 1 << kL2LgAssoc;
static const int kL2Assocmask = kL2Assoc - 1;
static const uint64 kL2Setmask = (1l << kL2LgSetsize) - 1;
static const uint64 kL2Tagmask = (1l << kL2LgLinesize) - 1;


static const int kL3LgSize = 21;
static const int kL3LgAssoc = 4;
static const int kL3LgLinesize = 6;
static const int kL3LgSetsize = kL3LgSize - kL3LgAssoc - kL3LgLinesize;
static const int kL3Setsize = 1 << kL3LgSetsize;
static const int kL3Assoc = 1 << kL3LgAssoc;
static const int kL3Assocmask = kL3Assoc - 1;
static const uint64 kL3Setmask = (1l << kL3LgSetsize) - 1;
static const uint64 kL3Tagmask = (1l << kL3LgLinesize) - 1;

static uint64 L1misses = 0;
static uint64 L2misses = 0;
static uint64 L3misses = 0;
static int L1rr = 0;
static int L2rr = 0;
static int L3rr = 0;

static uint64 L1tag[kL1Setsize * kL1Assoc];
static uint64 L2tag[kL2Setsize * kL2Assoc];
static uint64 L3tag[kL3Setsize * kL3Assoc];


void InitTags() {
  memset(L1tag, 0, kL1Setsize * kL1Assoc * sizeof(uint64));
  memset(L2tag, 0, kL2Setsize * kL2Assoc * sizeof(uint64));
  memset(L3tag, 0, kL3Setsize * kL3Assoc * sizeof(uint64));
  L1misses = L2misses = L3misses = 0;
}

#if TRACK_CACHES
bool L1(uint64 addr) {
  int set = ((addr >> kL1LgLinesize) & kL1Setmask) << kL1LgAssoc;
  uint64 tag = addr & ~kL1Tagmask;
  for (int i = 0; i < kL1Assoc; ++i) {
    if (L1tag[set + i] == tag) {return true;}
  }
  ++L1misses; 
  L1tag[set + L1rr] = tag;
  L1rr = (L1rr + 1) & kL1Assocmask;
  return false;
}

bool L2(uint64 addr) {
  int set = ((addr >> kL2LgLinesize) & kL2Setmask) << kL2LgAssoc;
  uint64 tag = addr & ~kL2Tagmask;
  for (int i = 0; i < kL2Assoc; ++i) {
    if (L2tag[set + i] == tag) {return true;}
  }
  ++L2misses; 
  L2tag[set + L2rr] = tag;
  L2rr = (L2rr + 1) & kL2Assocmask;
  return false;
}

bool L3(uint64 addr) {
#if HASHED_L3
  int set = (((addr >> kL3LgLinesize) ^ (addr >> kL3LgSize)) & kL3Setmask) << kL3LgAssoc;
#else
  int set = ((addr >> kL3LgLinesize) & kL3Setmask) << kL3LgAssoc;
#endif
  uint64 tag = addr & ~kL3Tagmask;
  for (int i = 0; i < kL3Assoc; ++i) {
    if (L3tag[set + i] == tag) {return true;}
  }
  ++L3misses; 
  L3tag[set + L3rr] = tag;
  L3rr = (L3rr + 1) & kL3Assocmask;
  return false;
}

void L123(uint64 addr) {
  L1(addr);
  L2(addr);
  L3(addr);
}

#else

bool L1(uint64 addr) {return false;}
bool L2(uint64 addr) {return false;}
bool L3(uint64 addr) {return false;}
void L123(uint64 addr) {}
#endif



// Give simple values near 1.0 to each element of arr
void SimpleInit(double* arr) {
  for (int row = 0; row < kRowsize; ++row) {
    for (int col = 0; col < kColsize; ++col) {
      arr[row * kRowsize + col] = 1.0 + (row * kRowsize + col) / 1000000.0;
    }
  }
}

// Zero arr
void ZeroInit(double* arr) {
  for (int row = 0; row < kRowsize; ++row) {
    for (int col = 0; col < kColsize; ++col) {
      arr[row * kRowsize + col] = 0.0;
    }
  }
}

// Sum all the elements of arr -- used for simple sameness check
double SimpleSum(double* arr) {
  double sum = 0.0;
  for (int row = 0; row < kRowsize; ++row) {
    for (int col = 0; col < kColsize; ++col) {
      sum += arr[row * kRowsize + col];
    }
  }
  return sum;
}

// Test two arrays for equality
bool EqualArray(const double* arr1, const double* arr2) {
  for (int k = 0; k < kRowsize * kColsize; ++k) {
    if (arr1[k] != arr2[k]) {return false;}
  }
  return true;
}

void TimeMe(const char* label, MulProc f, const double* a, const double* b, double* c) {
  InitTags();
  int64 start_usec = GetUsec();
  f(a, b, c);  
  int64 stop_usec = GetUsec();
  double duration_usec = stop_usec - start_usec;
  fprintf(stdout, "%s\t%5.3f seconds, sum=%18.9f\n", label, duration_usec/1000000.0, SimpleSum(c)); 
  fprintf(stdout, "Misses L1/L2/L3 %10lld %10lld %10lld\n", L1misses, L2misses, L3misses);
}



inline
double VectorSum1(const double* aptr, const double* bptr, int count, int rowsize) {
  const double* aptr2 = aptr;
  const double* bptr2 = bptr;
  double sum0 = 0.0;
  for (int k = 0; k < count; ++k) {
    sum0 += aptr2[0] * bptr2[0 * rowsize];
L1((uint64)&aptr2[0]);
L2((uint64)&aptr2[0]);
L3((uint64)&aptr2[0]);
L1((uint64)&bptr2[0 * rowsize]);
L2((uint64)&bptr2[0 * rowsize]);
L3((uint64)&bptr2[0 * rowsize]);
    aptr2 += 1;
    bptr2 += 1 * rowsize;
  }
  return (sum0);
}

inline
double VectorSum2(const double* aptr, const double* bptr, int count, int rowsize) {
  const double* aptr2 = aptr;
  const double* bptr2 = bptr;
  double sum0 = 0.0;
  double sum1 = 0.0;
  for (int k = 0; k < count; k += 2) {
    sum0 += aptr2[0] * bptr2[0 * rowsize];
    sum1 += aptr2[1] * bptr2[1 * rowsize];
L1((uint64)&aptr2[0]);
L2((uint64)&aptr2[0]);
L3((uint64)&aptr2[0]);
L1((uint64)&bptr2[0 * rowsize]);
L2((uint64)&bptr2[0 * rowsize]);
L3((uint64)&bptr2[0 * rowsize]);
L1((uint64)&aptr2[1]);
L2((uint64)&aptr2[1]);
L3((uint64)&aptr2[1]);
L1((uint64)&bptr2[1 * rowsize]);
L2((uint64)&bptr2[1 * rowsize]);
L3((uint64)&bptr2[1 * rowsize]);
    aptr2 += 2;
    bptr2 += 2 * rowsize;
  }
  return (sum0 + sum1);
}

inline
double VectorSum4(const double* aptr, const double* bptr, int count, int rowsize) {
  const double* aptr2 = aptr;
  const double* bptr2 = bptr;
  double sum0 = 0.0;
  double sum1 = 0.0;
  double sum2 = 0.0;
  double sum3 = 0.0;
  for (int k = 0; k < count; k += 4) {
    sum0 += aptr2[0] * bptr2[0 * rowsize];
    sum1 += aptr2[1] * bptr2[1 * rowsize];
    sum2 += aptr2[2] * bptr2[2 * rowsize];
    sum3 += aptr2[3] * bptr2[3 * rowsize];
L1((uint64)&aptr2[0]);
L2((uint64)&aptr2[0]);
L3((uint64)&aptr2[0]);
L1((uint64)&bptr2[0 * rowsize]);
L2((uint64)&bptr2[0 * rowsize]);
L3((uint64)&bptr2[0 * rowsize]);
L1((uint64)&aptr2[1]);
L2((uint64)&aptr2[1]);
L3((uint64)&aptr2[1]);
L1((uint64)&bptr2[1 * rowsize]);
L2((uint64)&bptr2[1 * rowsize]);
L3((uint64)&bptr2[1 * rowsize]);
L1((uint64)&aptr2[2]);
L2((uint64)&aptr2[2]);
L3((uint64)&aptr2[2]);
L1((uint64)&bptr2[2 * rowsize]);
L2((uint64)&bptr2[2 * rowsize]);
L3((uint64)&bptr2[2 * rowsize]);
L1((uint64)&aptr2[3]);
L2((uint64)&aptr2[3]);
L3((uint64)&aptr2[3]);
L1((uint64)&bptr2[3 * rowsize]);
L2((uint64)&bptr2[3 * rowsize]);
L3((uint64)&bptr2[3 * rowsize]);
    aptr2 += 4;
    bptr2 += 4 * rowsize;
  }
  return (sum0 + sum1 + sum2 + sum3);
}

//
//==============================================================================
//

void SimpleMultiply(const double* a, const double* b, double* c) {
  for (int row = 0; row < kRowsize; ++row) {
    for (int col = 0; col < kColsize; ++col) {
bool traceme = (col<2) & (row < 2);
traceme = false;
if (traceme) {fprintf(stdout, "[%d,%d] = ", row, col);}
      double sum = 0.0;
      for (int k = 0; k < kRowsize; ++k) {
        sum += a[row * kRowsize + k] * b[k * kRowsize + col];
int hit1a = L1((uint64)&a[row * kRowsize + k]);
int hit2a = L2((uint64)&a[row * kRowsize + k]);
int hit3a = L3((uint64)&a[row * kRowsize + k]);
int hit1b = L1((uint64)&b[k * kRowsize + col]);
int hit2b = L2((uint64)&b[k * kRowsize + col]);
int hit3b = L3((uint64)&b[k * kRowsize + col]);
if (traceme) {fprintf(stdout, "%016llx %016llx a%d%d%d b%d%d%d ", 
(uint64)&a[row * kRowsize + k], (uint64)&b[k * kRowsize + col],
hit1a, hit2a, hit3a, hit1b, hit2b, hit3b);}
      }
      c[row * kRowsize + col] = sum;
int hit1c = L1((uint64)&c[row * kRowsize + col]);
int hit2c = L2((uint64)&c[row * kRowsize + col]);
int hit3c = L3((uint64)&c[row * kRowsize + col]);
if (traceme) {fprintf(stdout, "c%d%d%d\n", hit1c, hit2c, hit3c);}
//if ((row < 16) && (col < 16)) {  
//fprintf(stdout, "[%d,%d] Misses L1/L2/L3 %10lld %10lld %10lld\n", row, col, L1misses, L2misses, L3misses);
//}
    }
  }
}

void SimpleMultiplyColumnwise(const double* a, const double* b, double* c) {
  for (int col = 0; col < kColsize; ++col) {
    for (int row = 0; row < kRowsize; ++row) {
bool traceme = (col<2) & (row < 2);
traceme = false;
if (traceme) {fprintf(stdout, "[%d,%d] = ", row, col);}
      double sum = 0.0;
      for (int k = 0; k < kRowsize; ++k) {
        sum += a[row * kRowsize + k] * b[k * kRowsize + col];
int hit1a = L1((uint64)&a[row * kRowsize + k]);
int hit2a = L2((uint64)&a[row * kRowsize + k]);
int hit3a = L3((uint64)&a[row * kRowsize + k]);
int hit1b = L1((uint64)&b[k * kRowsize + col]);
int hit2b = L2((uint64)&b[k * kRowsize + col]);
int hit3b = L3((uint64)&b[k * kRowsize + col]);
if (traceme) {fprintf(stdout, "%016llx %016llx a%d%d%d b%d%d%d ", 
(uint64)&a[row * kRowsize + k], (uint64)&b[k * kRowsize + col],
hit1a, hit2a, hit3a, hit1b, hit2b, hit3b);}
      }
      c[row * kRowsize + col] = sum;
int hit1c = L1((uint64)&c[row * kRowsize + col]);
int hit2c = L2((uint64)&c[row * kRowsize + col]);
int hit3c = L3((uint64)&c[row * kRowsize + col]);
if (traceme) {fprintf(stdout, "c%d%d%d\n", hit1c, hit2c, hit3c);}
//if ((row < 16) && (col < 16)) {  
//fprintf(stdout, "[%d,%d] Misses L1/L2/L3 %10lld %10lld %10lld\n", row, col, L1misses, L2misses, L3misses);
//}
    }
  }
}

// Just access 1 row and column, to time 1B pure multiplies. unroll to avoid dependant adds
void SimpleMultiplyOne(const double* a, const double* b, double* c) {
  for (int row = 0; row < kRowsize; ++row) {
    for (int col = 0; col < kColsize; ++col) {
      double sum0 = 0.0;
      double sum1 = 0.0;
      double sum2 = 0.0;
      double sum3 = 0.0;
      //for (int k = 0; k < kRowsize; ++k) {
      //  sum += a[(row * kRowsize + k) & 1] * b[(k * kRowsize + col) & 1];
      //}
      //c[(row * kRowsize + col) & 1] = sum;
      for (int k = 0; k < kRowsize; k += 4) {
        sum0 += a[0] * b[0];
        sum1 += a[1] * b[1];
        sum2 += a[2] * b[2];
        sum3 += a[3] * b[3];
      }
      c[1] = sum0 + sum1 + sum2 + sum3;
    }
  }
}


void SimpleMultiplyUnrolled4(const double* a, const double* b, double* c) {
  for (int row = 0; row < kRowsize; ++row) {
    for (int col = 0; col < kColsize; ++col) {
      c[row * kRowsize + col] = VectorSum4(&a[row * kRowsize + 0], 
					   &b[0 * kRowsize + col], 
					   kRowsize, kRowsize);
L1((uint64)&c[row * kRowsize + col]);
L2((uint64)&c[row * kRowsize + col]);
L3((uint64)&c[row * kRowsize + col]);
    }
  }
}

void SimpleMultiplyUnrolled2(const double* a, const double* b, double* c) {
  for (int row = 0; row < kRowsize; ++row) {
    for (int col = 0; col < kColsize; ++col) {
      c[row * kRowsize + col] = VectorSum2(&a[row * kRowsize + 0], 
					   &b[0 * kRowsize + col], 
					   kRowsize, kRowsize);
L1((uint64)&c[row * kRowsize + col]);
L2((uint64)&c[row * kRowsize + col]);
L3((uint64)&c[row * kRowsize + col]);
    }
  }
}

void SimpleMultiplyUnrolled1(const double* a, const double* b, double* c) {
  for (int row = 0; row < kRowsize; ++row) {
    for (int col = 0; col < kColsize; ++col) {
      c[row * kRowsize + col] = VectorSum1(&a[row * kRowsize + 0], 
					   &b[0 * kRowsize + col], 
					   kRowsize, kRowsize);
L1((uint64)&c[row * kRowsize + col]);
L2((uint64)&c[row * kRowsize + col]);
L3((uint64)&c[row * kRowsize + col]);
    }
  }
}

void PointerMultiplyUnrolled4(const double* a, const double* b, double* c) {
  const double* aptr = &a[0];
  const double* bptr = &b[0];
  for (int row = 0; row < kRowsize; ++row) {
    for (int col = 0; col < kColsize; ++col) {
      c[row * kRowsize + col] = VectorSum4(&a[row * kRowsize + 0], 
					   &b[0 * kRowsize + col], 
					   kRowsize, kRowsize);
L1((uint64)&c[row * kRowsize + col]);
L2((uint64)&c[row * kRowsize + col]);
L3((uint64)&c[row * kRowsize + col]);
    }
  }
}

// Depends on c being zero'd on entry
void BlockMultiply(const double* a, const double* b, double* c) {
  for (int row = 0; row < kRowsize; row += kBlocksize) {
    for (int col = 0; col < kColsize; col += kBlocksize) {
      // Calculate an 8x8 subarray of c
      for (int subcol = 0; subcol < kBlocksize; ++subcol) {
        for (int subrow = 0; subrow < kBlocksize; ++subrow) {
          c[(row + subrow) * kRowsize + (col + subcol)] += 
            VectorSum1(&a[(row + subrow) * kRowsize + 0], 
		       &b[0 * kRowsize + (col + subcol)], 
		       kRowsize, kRowsize);
L1((uint64)&c[(row + subrow) * kRowsize + (col + subcol)]);
L2((uint64)&c[(row + subrow) * kRowsize + (col + subcol)]);
L3((uint64)&c[(row + subrow) * kRowsize + (col + subcol)]);
        }
      }
    }
  }
}


// Depends on c being zero'd on entry
void BlockMultiplyPtrUnrolled4(const double* a, const double* b, double* c) {
  for (int row = 0; row < kRowsize; row += kBlocksize) {
    for (int col = 0; col < kColsize; col += kBlocksize) {
      // Calculate an 8x8 subarray of c
      for (int subrow = 0; subrow < kBlocksize; ++subrow) {
        for (int subcol = 0; subcol < kBlocksize; ++subcol) {
          c[(row + subrow) * kRowsize + (col + subcol)] += 
            VectorSum4(&a[(row + subrow) * kRowsize + 0], 
		       &b[0 * kRowsize + (col + subcol)], 
		       kRowsize, kRowsize);
L1((uint64)&c[(row + subrow) * kRowsize + (col + subcol)]);
L2((uint64)&c[(row + subrow) * kRowsize + (col + subcol)]);
L3((uint64)&c[(row + subrow) * kRowsize + (col + subcol)]);
        }
      }
    }
  }
}


// Copy an NxN subarray to linear addresses, spreading across all L1 cache sets
// 8x8   => 64*8 bytes = 512 bytes or 8 sequential cache lines
// 16x16 => 256*8  = 2048 bytes or 32 sequential cache lines
// 32x32 => 1024*8 = 8192 bytes or 128 sequential cache lines (two lines per set in i3 L1 cache)
void Remap(const double* x, double* xprime) {
  int k = 0;
  for (int row = 0; row < kRemapsize; ++row) {
    for (int col = 0; col < kRemapsize; col += 4) {
      xprime[k + 0] = x[row * kRowsize + col + 0];
      xprime[k + 1] = x[row * kRowsize + col + 1];
      xprime[k + 2] = x[row * kRowsize + col + 2];
      xprime[k + 3] = x[row * kRowsize + col + 3];
L1((uint64)&xprime[k + 0]);
L1((uint64)&xprime[k + 1]);
L1((uint64)&xprime[k + 2]);
L1((uint64)&xprime[k + 3]);
L1((uint64)&x[row * kRowsize + col + 0]);
L1((uint64)&x[row * kRowsize + col + 1]);
L1((uint64)&x[row * kRowsize + col + 2]);
L1((uint64)&x[row * kRowsize + col + 3]);

L2((uint64)&xprime[k + 0]);
L2((uint64)&xprime[k + 1]);
L2((uint64)&xprime[k + 2]);
L2((uint64)&xprime[k + 3]);
L2((uint64)&x[row * kRowsize + col + 0]);
L2((uint64)&x[row * kRowsize + col + 1]);
L2((uint64)&x[row * kRowsize + col + 2]);
L2((uint64)&x[row * kRowsize + col + 3]);

L3((uint64)&xprime[k + 0]);
L3((uint64)&xprime[k + 1]);
L3((uint64)&xprime[k + 2]);
L3((uint64)&xprime[k + 3]);
L3((uint64)&x[row * kRowsize + col + 0]);
L3((uint64)&x[row * kRowsize + col + 1]);
L3((uint64)&x[row * kRowsize + col + 2]);
L3((uint64)&x[row * kRowsize + col + 3]);

      k += 4;
    }
  }
}

// Copy all NxN subarrays to linear addresses
void RemapAll(const double* x, double* xprime) {
  int k = 0;
  for (int row = 0; row < kRowsize; row += kRemapsize) {
    for (int col = 0; col < kColsize; col += kRemapsize) {
      Remap(&x[row * kRowsize + col], &xprime[k]);
      k += (kRemapsize * kRemapsize);
    }
  }
}

// Copy an NxN subarray from linear addresses
void UnRemap(const double* xprime, double* x) {
  int k = 0;
  for (int row = 0; row < kRemapsize; ++row) {
    for (int col = 0; col < kRemapsize; col += 4) {
      x[row * kRowsize + col + 0] = xprime[k + 0];
      x[row * kRowsize + col + 1] = xprime[k + 1];
      x[row * kRowsize + col + 2] = xprime[k + 2];
      x[row * kRowsize + col + 3] = xprime[k + 3];
L1((uint64)&x[row * kRowsize + col + 0]);
L1((uint64)&x[row * kRowsize + col + 1]);
L1((uint64)&x[row * kRowsize + col + 2]);
L1((uint64)&x[row * kRowsize + col + 3]);
L1((uint64)&xprime[k + 0]);
L1((uint64)&xprime[k + 1]);
L1((uint64)&xprime[k + 2]);
L1((uint64)&xprime[k + 3]);

L2((uint64)&x[row * kRowsize + col + 0]);
L2((uint64)&x[row * kRowsize + col + 1]);
L2((uint64)&x[row * kRowsize + col + 2]);
L2((uint64)&x[row * kRowsize + col + 3]);
L2((uint64)&xprime[k + 0]);
L2((uint64)&xprime[k + 1]);
L2((uint64)&xprime[k + 2]);
L2((uint64)&xprime[k + 3]);

L3((uint64)&x[row * kRowsize + col + 0]);
L3((uint64)&x[row * kRowsize + col + 1]);
L3((uint64)&x[row * kRowsize + col + 2]);
L3((uint64)&x[row * kRowsize + col + 3]);
L3((uint64)&xprime[k + 0]);
L3((uint64)&xprime[k + 1]);
L3((uint64)&xprime[k + 2]);
L3((uint64)&xprime[k + 3]);

      k += 4;
    }
  }
}

// Copy all NxN subarrays from linear addresses
void UnRemapAll(const double* xprime, double* x) {
  int k = 0;
  for (int row = 0; row < kRowsize; row += kRemapsize) {
    for (int col = 0; col < kColsize; col += kRemapsize) {
      UnRemap(&xprime[k], &x[row * kRowsize + col]);
      k += (kRemapsize * kRemapsize);
    }
  }
}

// Transpose matrix
void TransposeAll(const double* x, double* xprime) {
  for (int row = 0; row < kRowsize; ++row) {
    for (int col = 0; col < kColsize; ++col) {
      xprime[col * kRowsize + row] = x[row * kRowsize + col];
L1((uint64)&x[row * kRowsize + col]);
L2((uint64)&x[row * kRowsize + col]);
L3((uint64)&x[row * kRowsize + col]);
L1((uint64)&xprime[col * kRowsize + row]);
L2((uint64)&xprime[col * kRowsize + row]);
L3((uint64)&xprime[col * kRowsize + row]);
    }
  }
}

// Transpose one block
void BlockTranspose(const double* x, double* xprime) {
  for (int row = 0; row < kBlocksize; ++row) {
    for (int col = 0; col < kBlocksize; col += 4) {
      xprime[(col + 0) * kRowsize + row] = x[row * kRowsize + col + 0];
      xprime[(col + 1) * kRowsize + row] = x[row * kRowsize + col + 1];
      xprime[(col + 2) * kRowsize + row] = x[row * kRowsize + col + 2];
      xprime[(col + 3) * kRowsize + row] = x[row * kRowsize + col + 3];

L1((uint64)&x[row * kRowsize + col + 0]);
L2((uint64)&x[row * kRowsize + col + 0]);
L3((uint64)&x[row * kRowsize + col + 0]);
L1((uint64)&xprime[(col + 0) * kRowsize + row]);
L2((uint64)&xprime[(col + 0) * kRowsize + row]);
L3((uint64)&xprime[(col + 0) * kRowsize + row]);

L1((uint64)&x[row * kRowsize + col + 1]);
L2((uint64)&x[row * kRowsize + col + 1]);
L3((uint64)&x[row * kRowsize + col + 1]);
L1((uint64)&xprime[(col + 1) * kRowsize + row]);
L2((uint64)&xprime[(col + 1) * kRowsize + row]);
L3((uint64)&xprime[(col + 1) * kRowsize + row]);

L1((uint64)&x[row * kRowsize + col + 2]);
L2((uint64)&x[row * kRowsize + col + 2]);
L3((uint64)&x[row * kRowsize + col + 2]);
L1((uint64)&xprime[(col + 2) * kRowsize + row]);
L2((uint64)&xprime[(col + 2) * kRowsize + row]);
L3((uint64)&xprime[(col + 2) * kRowsize + row]);

L1((uint64)&x[row * kRowsize + col + 3]);
L2((uint64)&x[row * kRowsize + col + 3]);
L3((uint64)&x[row * kRowsize + col + 3]);
L1((uint64)&xprime[(col + 3) * kRowsize + row]);
L2((uint64)&xprime[(col + 3) * kRowsize + row]);
L3((uint64)&xprime[(col + 3) * kRowsize + row]);

    }
  }
}

// Block Transpose matrix
void BlockTransposeAll(const double* x, double* xprime) {
  for (int row = 0; row < kRowsize; row += kBlocksize) {
    for (int col = 0; col < kColsize; col += kBlocksize) {
      BlockTranspose(&x[row * kRowsize + col], &xprime[col * kRowsize + row]);
    }
  }
}


// Remap input arrays to spread Remap blocks across successive cache lines,
// multiply, then remap output
// Depends on c being zero'd on entry
void BlockMultiplyRemap(const double* a, const double* b, double* c) {
  RemapAll(a, aa);
  RemapAll(b, bb);
#if 1
  for (int row = 0; row < kRowsize; row += kRemapsize) {
    for (int col = 0; col < kColsize; col += kRemapsize) {
      // cc block starts at row * kRowsize + col * kRemapsize
      double* ccptr = &cc[(row * kRowsize) + (col * kRemapsize)];     

      for (int k = 0; k < kRowsize; k += kRemapsize) {
        // aa block starts at row * kRowsize + k * kRemapsize 
        // bb block starts at(k * kRowsize + col * kRemapsize
        const double* aaptr = &aa[(row * kRowsize) + (k * kRemapsize)];
        const double* bbptr = &bb[(k * kRowsize) + (col * kRemapsize)];

        // Calculate an NxN subarray of c
        int kk = 0;
        for (int subrow = 0; subrow < kRemapsize; ++subrow) {
          for (int subcol = 0; subcol < kRemapsize; ++subcol) {
            ccptr[kk] += VectorSum4(&aaptr[subrow * kRemapsize + 0],
                                    &bbptr[0 * kRemapsize + subcol],
		                    kRemapsize, kRemapsize);
L1((uint64)&ccptr[kk]);
L2((uint64)&ccptr[kk]);
L3((uint64)&ccptr[kk]);
            ++kk;
          }
        }
      }
    }
  }
#endif
  RemapAll(cc, c);
}


// Transpose second input array to be in column-major order
void SimpleMultiplyTranspose(const double* a, const double* b, double* c) {
  TransposeAll(b, bb);
  for (int row = 0; row < kRowsize; ++row) {
    for (int col = 0; col < kColsize; ++col) {
      c[row * kRowsize + col] = VectorSum1(&a[row * kRowsize + 0], 
					   &bb[col * kRowsize + 0], 
					   kRowsize, 1);
L1((uint64)&c[row * kRowsize + col]);
L2((uint64)&c[row * kRowsize + col]);
L3((uint64)&c[row * kRowsize + col]);
    }
  }
}

// Transpose second input array to be in column-major order
void SimpleMultiplyTransposeFast(const double* a, const double* b, double* c) {
  BlockTransposeAll(b, bb);
  for (int row = 0; row < kRowsize; ++row) {
    for (int col = 0; col < kColsize; ++col) {
      c[row * kRowsize + col] = VectorSum4(&a[row * kRowsize + 0], 
					   &bb[col * kRowsize + 0], 
					   kRowsize, 1);
L1((uint64)&c[row * kRowsize + col]);
L2((uint64)&c[row * kRowsize + col]);
L3((uint64)&c[row * kRowsize + col]);
    }
  }
}


double* PageAlign(double* p) {
  double* p_local = p + 511;
  *reinterpret_cast<uint64*>(&p_local) &= ~0xfff;
////  fprintf(stdout, "%016llx %016llx\n", (uint64)p, (uint64)p_local);
  return p_local;
}

int main(int argc, const char** argv) {
kutrace::mark_a("alloc");
  double* abase = new double[kRowsize * kColsize + 512];
  double* bbase = new double[kRowsize * kColsize + 512];
  double* cbase = new double[kRowsize * kColsize + 512];
  double* a = PageAlign(abase);
  double* b = PageAlign(bbase);
  double* c = PageAlign(cbase);
  double* aabase = new double[kRowsize * kColsize + 512];
  double* bbbase = new double[kRowsize * kColsize + 512];
  double* ccbase = new double[kRowsize * kColsize + 512];
  aa = PageAlign(aabase);
  bb = PageAlign(bbbase);
  cc = PageAlign(ccbase);

kutrace::mark_a("init");
  SimpleInit(a);
  SimpleInit(b);
  InitTags();

  // Test remap
kutrace::mark_a("remap");
  RemapAll(a, aa);
  UnRemapAll(aa, c);
  fprintf(stdout, "a  sum=%18.9f\n", SimpleSum(a)); 
  fprintf(stdout, "aa sum=%18.9f\n", SimpleSum(aa)); 
  fprintf(stdout, "c  sum=%18.9f\n", SimpleSum(c)); 
  fprintf(stdout, "%s\n", EqualArray(a, c) ? "Equal" : "Not equal"); 
  fprintf(stdout, "Remap Misses L1/L2/L3 %10lld %10lld %10lld\n", L1misses, L2misses, L3misses);
  InitTags();

  // Test transpose
kutrace::mark_a("trans");
  TransposeAll(b, bb);
  TransposeAll(bb, c);
  fprintf(stdout, "b  sum=%18.9f\n", SimpleSum(b)); 
  fprintf(stdout, "bb sum=%18.9f\n", SimpleSum(bb)); 
  fprintf(stdout, "c  sum=%18.9f\n", SimpleSum(c)); 
  fprintf(stdout, "%s\n", EqualArray(b, c) ? "Equal" : "Not equal"); 
  fprintf(stdout, "Transpose Misses L1/L2/L3 %10lld %10lld %10lld\n", L1misses, L2misses, L3misses);
  InitTags();

kutrace::mark_a("btrans");
  BlockTransposeAll(b, bb);
  BlockTransposeAll(bb, c);
  fprintf(stdout, "b  sum=%18.9f\n", SimpleSum(b)); 
  fprintf(stdout, "bb sum=%18.9f\n", SimpleSum(bb)); 
  fprintf(stdout, "c  sum=%18.9f\n", SimpleSum(c)); 
  fprintf(stdout, "%s\n", EqualArray(b, c) ? "Equal" : "Not equal"); 
  fprintf(stdout, "BlockTranspose Misses L1/L2/L3 %10lld %10lld %10lld\n", L1misses, L2misses, L3misses);
  InitTags();


kutrace::mark_a("simp");
  TimeMe("SimpleMultiply            ", SimpleMultiply, a, b, c);
kutrace::mark_a("simpc");
  TimeMe("SimpleMultiplyColumnwise  ", SimpleMultiplyColumnwise, a, b, c);

#if 0
  TimeMe("SimpleMultiplyUnrolled1   ", SimpleMultiplyUnrolled1, a, b, c);
  TimeMe("SimpleMultiplyUnrolled2   ", SimpleMultiplyUnrolled2, a, b, c);
  TimeMe("SimpleMultiplyUnrolled4   ", SimpleMultiplyUnrolled4, a, b, c);
  TimeMe("PointerMultiplyUnrolled4  ", PointerMultiplyUnrolled4, a, b, c);

  ZeroInit(c);
  TimeMe("BlockMultiply             ", BlockMultiply, a, b, c);
  ZeroInit(c);
  TimeMe("BlockMultiplyPtrUnrolled4 ", BlockMultiplyPtrUnrolled4, a, b, c);
#endif

kutrace::mark_a("simpt");
  TimeMe("SimpleMultiplyTranspose   ", SimpleMultiplyTranspose, a, b, c);
  ZeroInit(c);
kutrace::mark_a("simptf");
  TimeMe("SimpleMultiplyTransposeFast", SimpleMultiplyTransposeFast, a, b, c);
  ZeroInit(c);
kutrace::mark_a("simpr");
  TimeMe("BlockMultiplyRemap        ", BlockMultiplyRemap, a, b, c);
  ZeroInit(c);
kutrace::mark_a("simp1");
  TimeMe("IGNORE SimpleMultiplyOne     ", SimpleMultiplyOne, a, b, c);


  delete[] ccbase; 
  delete[] bbbase; 
  delete[] aabase; 
  delete[] cbase; 
  delete[] bbase; 
  delete[] abase; 
  return 0;
}

