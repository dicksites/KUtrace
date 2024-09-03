// fancylock2.cc 
//
// This defines a software lock that includes some statistics and some identification
//
// Copyright 2021 Richard L. Sites

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "basetypes.h"
#include "fancylock2.h"

//
// Fancylock2 (64 bytes, cacheline aligned)
//
//    +-------+-------+-------+-------+-------+-------+-------+-------+
//  0 |             lock              |           waiters             |
//    +-------+-------+-------+-------+-------+-------+-------+-------+
//  8 |                          wait.counts                          |
//    +-------+-------+-------+-------+-------+-------+-------+-------+
// 16 |                          wait.counts_hi                       |
//    +-------+-------+-------+-------+-------+-------+-------+-------+
// 24 | hmin  | hmax  |expec'd| ///// |            holder             |
//    +-------+-------+-------+-------+-------+-------+-------+-------+
// 32 | ///////////////////////////// | ///////////////////////////// |                                                              |
//    +-------+-------+-------+-------+-------+-------+-------+-------+
// 40 |   lnamehash   |           filename
//    +-------+-------+-------+-------+-------+-------+-------+-------+
// 48 |                           filename                            |
//    +-------+-------+-------+-------+-------+-------+-------+-------+
// 56 |                         filename:line                         |
//    +-------+-------+-------+-------+-------+-------+-------+-------+
//

static const int32 kBucketWidthBits[8] = {13, 11, 10, 8,  7, 6, 5, 4};
static const int32 kBucketStartBit[8] = {0, 13, 24, 34,   42, 49, 55, 60};
static const uint64 kBucketIncr[8] =
  {0x0000000000000001LLU, 0x0000000000002000LLU, 
   0x0000000001000000LLU, 0x0000000400000000LLU,
   0x0000040000000000LLU, 0x0002000000000000LLU, 
   0x0080000000000000LLU, 0x1000000000000000LLU};
static const uint64 kBucketField[8] =
  {0x0000000000001FFFLLU, 0x0000000000FFE000LLU,
   0x00000003FF000000LLU, 0x000003FC00000000LLU,
   0x0001FC0000000000LLU, 0x007E000000000000LLU,
   0x0F80000000000000LLU, 0xF000000000000000LLU};
static const uint64 kBucketHigh[8]  = 
  {0x0000000000001000LLU, 0x0000000000800000LLU,
   0x0000000200000000LLU, 0x0000020000000000LLU,
   0x0001000000000000LLU, 0x0040000000000000LLU,
   0x0800000000000000LLU, 0x8000000000000000LLU};

static const uint64 kBucketAllLow = 
   0x0000000000000001LLU | 0x0000000000002000LLU | 
   0x0000000001000000LLU | 0x0000000400000000LLU |
   0x0000040000000000LLU | 0x0002000000000000LLU | 
   0x0080000000000000LLU | 0x1000000000000000LLU;

// Upper value of each histogram bucket for power-of-ten buckets
static const uint32 kWaitMaxes[8] = {9, 99, 999, 9999,   99999, 999999, 9999999, 0x7FFFFFFF};


// These tables let us map integer values up to about 100M into single bytes and back out
// with better than 10% accuracy. This is good enough for 1-2 digits precision 
// of 90th percentile microsecond delay.

// log10(n) as 3.5 bits rounded, [0..255], e.g. log10(255)=2.4065 x32 = 77.008
static const uint8 CACHEALIGNED kLog10As3dot5[256] = {   
   0,  1, 10, 15, 19, 22, 25, 27,   29, 31, 32, 33, 35, 36, 37, 38,   
  39, 39, 40, 41, 42, 42, 43, 44,   44, 45, 45, 46, 46, 47, 47, 48,   
  48, 49, 49, 49, 50, 50, 51, 51,   51, 52, 52, 52, 53, 53, 53, 54,   
  54, 54, 54, 55, 55, 55, 55, 56,   56, 56, 56, 57, 57, 57, 57, 58, 
  58, 58, 58, 58, 59, 59, 59, 59,   59, 60, 60, 60, 60, 60, 61, 61,   
  61, 61, 61, 61, 62, 62, 62, 62,   62, 62, 63, 63, 63, 63, 63, 63,   
  63, 64, 64, 64, 64, 64, 64, 64,   65, 65, 65, 65, 65, 65, 65, 65,   
  66, 66, 66, 66, 66, 66, 66, 66,   67, 67, 67, 67, 67, 67, 67, 67, 
  
  67, 68, 68, 68, 68, 68, 68, 68,   68, 68, 68, 69, 69, 69, 69, 69,   
  69, 69, 69, 69, 69, 70, 70, 70,   70, 70, 70, 70, 70, 70, 70, 70,   
  71, 71, 71, 71, 71, 71, 71, 71,   71, 71, 71, 71, 72, 72, 72, 72,   
  72, 72, 72, 72, 72, 72, 72, 72,   72, 73, 73, 73, 73, 73, 73, 73,
  73, 73, 73, 73, 73, 73, 73, 74,   74, 74, 74, 74, 74, 74, 74, 74,   
  74, 74, 74, 74, 74, 75, 75, 75,   75, 75, 75, 75, 75, 75, 75, 75,   
  75, 75, 75, 75, 75, 76, 76, 76,   76, 76, 76, 76, 76, 76, 76, 76,   
  76, 76, 76, 76, 76, 76, 77, 77,   77, 77, 77, 77, 77, 77, 77, 77,   
};   

// pow10(n/32), n in [0..31] 
// Table values are 4.4 bits, 0.0 .. 9.3057
// e.g. pow10(31/32) = 9.3057 x16 = 149
static const uint8 kPow10As4dot4[32] = {  
  16, 17, 18, 20, 21, 23, 25, 26,
  28, 31, 33, 35, 38, 41, 44, 47,
  51, 54, 58, 63, 67, 73, 78, 84,
  90, 97, 104, 112, 120, 129, 139, 149
};

inline uint8 min_uint8(uint8 a, uint8 b) {return (a < b) ? a : b;}
inline uint8 max_uint8(uint8 a, uint8 b) {return (a > b) ? a : b;}

// Quick hash of 24 byte string into low 16 bits of uint64
// Constants from murmur3
inline uint16 Hash16(const char* str) {
  const uint64* str64 = reinterpret_cast<const uint64*>(str);
  uint64 hash = (str64[0] * 0xff51afd7ed558ccdLLU) + 
                (str64[1] * 0xc4ceb9fe1a85ec53LLU) +
                (str64[2] * 0xff51afd7ed558ccdLLU);
  hash ^= (hash >> 32);
  hash ^= (hash >> 16);
  uint16 hash16 = hash;		// Truncates to 16 bits
  return hash16;
}

// Return log base 10 of val as a 3.5 fixed-point byte. 
// log10 as 3.5 bits 0.0 .. 7.31 (7.96875) = 1.0 .. 93,057,204 in steps of 1.0746x
uint8 Log10As3dot5(uint32 val) {
  if (val > 93057204) {return 255;}
  // Table lookup of divisor could be faster, but multiple divides cannot happen often 
  //   (<400 per second when over 2550 usec each)
  uint8 n = 0;
  while (val > 2550) {val /= 100; n += 2*32;}
  if (val > 255) {val /= 10; n += 1*32;}
  return n + kLog10As3dot5[val];  // We trade up to 4 cache lines for no conditional branches here.
}

// Input is xxx.yyyyy as eight bits in a single byte
// Return (10 ** xxx) * (10 ** 0.yyyyy)
// Smallest possible non-zero value is Log10byteToFloat(1) ==> 1.06250
// Largest possible value is           Log10byteToFloat(255) ==> 93125000.0
float Log10byteToFloat(uint8 xxxyyyyy) {
  if (xxxyyyyy == 0) {return 0.0;}
  float retval = 1.0;
  int xxx = xxxyyyyy >> 5;
  int yyyyy = xxxyyyyy & 0x1F;
  while (xxx > 0) {retval *= 10.0; --xxx;}
  return retval * (kPow10As4dot4[yyyyy] / 16.0);
}

int Log10byteToInt(uint8 xxxyyyyy) {
  int retval = roundf(Log10byteToFloat(xxxyyyyy));
  return retval;
}



// Called infrequently, so not performance critical
inline uint64 GetField(uint64 counts, int i) {
  return (counts & kBucketField[i]) >> kBucketStartBit[i];
}

// Called infrequently, so not performance critical
void UnpackCounts(const FancyLock2::CheapHist2* ch, uint32* bucketcounts) {
  for (int i = 0; i < 8; ++i) {
    bucketcounts[i] = GetField(ch->counts, i);
    bucketcounts[i] += GetField(ch->counts_hi, i) << kBucketWidthBits[i];
  }
}

// Return percentile fractional location in 8 buckets as a 3.5 fixed-point byte. 
// E.g. for bucketcounts[8] = {10, 20, 10, 8, 8, 0, 0, 0} 90th percentile 50.4 of 56 
//  is bucket[4.25] x32 = 136   
//
// We map into a linear fraction and do the log mapping on the way out
// If we use half the counts in a 10x bucket, assume it lands at 10 ** 0.5 = 3.16,
//  not at 10/2 = 5. Thisis a better match to likely declining tail above 80th %ile
// 0.1 ==> 1.26, 0.5 ==> 3.16, 0.9 ==> 7.94  in 1..10 bucket
// Averaging bucket fractions then 10**x will produce geometric mean, not arithmetic
//
// Called infrequently, so not performance critical
//
// Percentile ranges from 0.0 to 1.0, not 0-100
uint8 FindPercentileBucket(float percentile, const uint32* bucketcounts, 
                           const FancyLock2::CheapHist2* ch) {
  uint32 totalcount = 0;
  for (int i = 0; i < 8; ++i) {totalcount  += bucketcounts[i];}
  if (totalcount == 0) {return 0;}
  if (percentile <= 0.0001) {return ch->hmin;}
  if (percentile > 0.9999) {return ch->hmax;}

  float goalcount = totalcount  * percentile;

  // We want bucket.fraction that gives 
  //   goalcount <= running counts up to and including that bucket 
  float runningcount = 0;
  int k = 0;
  while ((runningcount + bucketcounts[k]) < goalcount) {
    runningcount += bucketcounts[k++]; 	// Completely include bucket k
  }
  // At this point, 
  //   runningcount thru bucket k-1 is < goalcount and
  //   runningcount + bucketcounts[k] >= goalcount and
  //   k is in 0..7
  // We want to find the closest fraction of bucket k that approximates
  // the percentile value.
  // Most buckets have 32 choices of fraction, 0..31, but the first and last
  // buckets are bounded by the hmin and hmax log values seen
  float remainder = goalcount - runningcount;
  uint8 lo = max_uint8(k * 32, ch->hmin) & 31;
  uint8 hi = min_uint8(k * 32 + 31, ch->hmax) & 31;
  // Interpolate, assuming total items in bucket are uniformly distributed in fractions [lo..hi)
  float fraction = (hi + 1 - lo) * (remainder / bucketcounts[k]);
  int ifraction = fraction;	// truncates
  // Note: if we need all of topmost bucket, sum can be 256. Return 255 in that case.
  int retval = k * 32 + (lo + ifraction);
  if (retval > 255) {retval = 255;}
  return retval;	// Truncates to 8 bits
}

// Calc 90th percentile from histogram counts and then zero histogram
// Called infrequently, so not performance critical
int Calc90ile(const FancyLock2::CheapHist2* ch) {
  if ((ch->counts == 0) && (ch->counts_hi == 0)) {return 0;}	// We never started
  uint32 bucketcounts[8];
  UnpackCounts(ch, bucketcounts);
  uint8 percentile90 = FindPercentileBucket(0.90, bucketcounts, ch);
  return Log10byteToInt(percentile90);
}

void DumpCheapHist2(FILE* f, FancyLock2::CheapHist2* ch) {
  uint32 bucketcounts[8];
  UnpackCounts(ch, bucketcounts);
  uint32 sum = 0;

  fprintf(f, "  1us 10 100    1ms 10 100    1s 10\n");
  fprintf(f, " [");
  for (int i = 0; i < 8; ++i) {
    fprintf(f, "%u ", bucketcounts[i]);
    if ((i % 3) == 2) {fprintf(f, "  ");}
    sum += bucketcounts[i];
  }
  fprintf(f, "] sum = %u\n", sum);

  fprintf(f, "  Minimum   %5d us\n",  Log10byteToInt(ch->hmin));
  fprintf(f, "  Maximum   %5d us\n",  Log10byteToInt(ch->hmax));
  fprintf(f, "  90th %%ile %5d us\n", Calc90ile(ch));
  fprintf(f, "  Expected  %5d us\n",  Log10byteToInt(ch->expected));
}

void DumpFancyLock2Struct(FILE* f, FancyLock2::FancyLock2Struct* fl) {
  fprintf(f, "  Struct %s [%04x] %08x %08x\n", 
          fl->filename, fl->lnamehash, fl->lock, fl->waiters);
  DumpCheapHist2(f, &fl->wait);
  fprintf(f, "\n");
}

void DumpBuckets(FILE* f, FancyLock2::CheapHist2* ch) {
  fprintf(f, "lo: ");
  for (int i = 0; i < 8; ++i) {
    int temp = GetField(ch->counts, i);
    fprintf(f, "[%d]%d ", i, temp);
  }
  fprintf(f, "   ");
  fprintf(f, "hi: ");
  for (int i = 0; i < 8; ++i) {
    int temp = GetField(ch->counts_hi, i);
    fprintf(f, "[%d]%d ", i, temp);
  }
  fprintf(f, "\n");
}

// We just incremented bucket bkt and it overflowed.
// First subtract back tokill carry into next bucket, then
// zero this bucket and increment count_hi bucket.
// If that overflows, halve all the counts (exponential decay over minutes to hours)
// Called infrequently; not performance critical
void Overflow(FancyLock2::CheapHist2* ch, int bkt) {
//fprintf(stderr, "Overflow carry[%d]\n", bkt);
  // Correct the overflow increment
  ch->counts -= kBucketIncr[bkt];	// Take out the increment
  ch->counts &= ~kBucketField[bkt];	// Zero the field
  ch->counts_hi += kBucketIncr[bkt];	// Carry into high bits
  if ((ch->counts_hi & kBucketField[bkt]) == 0) {
fprintf(stderr, "\nOverflow[%d] halving the counts\n", bkt);
    // Correct the hi overflow increment
    ch->counts_hi -= kBucketIncr[bkt];		// Take out the increment
    ch->counts_hi &= ~kBucketField[bkt];	// Zero the field
    // Halve all the low counts
    ch->counts &= ~kBucketAllLow;	// Zero low bit of each bucket
    ch->counts >>= 1;			// Halve low part
    // Move low bits of high half to high bits of low half
    for (int i = 0; i < 8; ++i) {
      if ((ch->counts_hi & kBucketIncr[i]) != 0) {
        ch->counts |= kBucketHigh[i];
      }
    }
    ch->counts_hi &= ~kBucketAllLow;	// Zero low bit of each bucket
    ch->counts_hi >>= 1;		// Halve high part
    ch->counts_hi |= kBucketHigh[bkt];  // After shift, overflowed bucket = 1000...
//DumpBuckets(stderr, ch);
fprintf(stderr, "after  "); DumpCheapHist2(stderr, ch);
  }
}

// Binary search of 8 bucket maximums
int FindSubscr(uint32 val, const uint32* maxes) {
  if (val <= maxes[3]) {
    if (val <= maxes[1]) {
      return (val <= maxes[0]) ? 0 : 1;
    } else {
      return (val <= maxes[2]) ? 2 : 3;
    }
  } else {
      if (val <= maxes[5]) {
      return (val <= maxes[4]) ? 4 : 5;
      } else {
      return (val <= maxes[6]) ? 6 : 7;
      }
  }
}


//---------------------------------------------------------------------------//
// Exported routines                                                         //
//---------------------------------------------------------------------------//

// Constructor
// Last parameter allows distinctive name init in array of locks for 0<subline
FancyLock2::FancyLock2(const char* filename, const int linenum, 
                       const int expected_wait_usec, const int subline) {
  memset(&fancy2struct_, 0, sizeof(FancyLock2Struct));
  fancy2struct_.holder = 0x80000000;	// No holder
  // Form filename:line to fit into 22 bytes, truncating on left as needed
  int len = strlen(filename);
  const char* filestart = (len < 22) ? filename : filename + len - 22;
  char buffer[64];
  if (0 < subline) {
    len = snprintf(buffer, 64, "%s:%d_%d", filestart, linenum, subline);
  } else {
    len = snprintf(buffer, 64, "%s:%d", filestart, linenum);
  } 
  if (len < 22) {
    strcpy(fancy2struct_.filename, buffer);
  } else {
    // Last 21 chars
    memcpy(&fancy2struct_.filename, buffer + len - 21, 21);
  }
  // fancy2struct_.filename[21] = '\0';	memset above
  // fancy2struct_.lnamehash = 0;	memset above
  // This hashes 24 bytes; depends on lnamehash=0 before call
  fancy2struct_.lnamehash = Hash16((const char*)(&fancy2struct_.lnamehash));

  fancy2struct_.wait.hmin = 255;
  // fancy2struct_.wait.hmax = 0;	memset above
  fancy2struct_.wait.expected =  Log10As3dot5(expected_wait_usec);
  fprintf(stderr, "Fancylock2(ex=%dus) [%04x] at %s\n", 
          expected_wait_usec, fancy2struct_.lnamehash, fancy2struct_.filename);
  ////DumpFancyLock2Struct(stderr, &fancy2struct_);
}

// Destructor. Print final stats to stderr
FancyLock2::~FancyLock2() {
  if (fancy2struct_.wait.hmin > fancy2struct_.wait.hmax) {
    fprintf(stderr, "[%s] zero entries\n",  fancy2struct_.filename);
    return;
  } 

  int i90ile =   Calc90ile(&fancy2struct_.wait);
  int expected = Log10byteToInt(fancy2struct_.wait.expected);
  fprintf(stderr, "[%s]%s\n",  fancy2struct_.filename,
    (i90ile > expected) ? " ERROR: 90%ile > EXPECTED" : "");

  DumpCheapHist2(stderr, &fancy2struct_.wait);
}

// Export current 90th percentile acquire time (usec)
int FancyLock2::Get90ile() {
  return Calc90ile(&fancy2struct_.wait);
}

// Record waiting time and queue depth. Takes about 10-15 nsec on Intel i3 7100.
// Called fairly frequently
void FancyLock2::IncrCounts(uint32 wait_us) {
  //VERYTEMP
  //fprintf(stdout, "[%s] IncrCounts(%dus, %dq)\n", 
  //        fancy2struct_.filename, wait_us, queue_depth);
 
  // Remember min and max values
  uint8 waitbyte = Log10As3dot5(wait_us);
  fancy2struct_.wait.hmin = min_uint8(fancy2struct_.wait.hmin, waitbyte);
  fancy2struct_.wait.hmax = max_uint8(fancy2struct_.wait.hmax, waitbyte);
  // Increment wait histogram, bucket number bkt
  int bkt = FindSubscr(wait_us , kWaitMaxes);
  fancy2struct_.wait.counts += kBucketIncr[bkt];
  // If field is full, overflow into count_hi
  if ((fancy2struct_.wait.counts & kBucketField[bkt]) == 0) {
    Overflow(&fancy2struct_.wait, bkt);
  }

}

