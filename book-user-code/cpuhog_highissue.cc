/* Bastardized from Jenkins hash (subset aligned 32-bit words) */
/* http://www.burtleburtle.net/bob/hash/doobs.html */
/* Chosen only because it fills up issue slots without much memory activity */
/* If you want a modern hash, look into murmur hash */
// Copyright 2021 Richard L. Sites

#include <stdio.h>
#include <sys/time.h>	/* gettimeofday */

typedef  unsigned long int uint64;
typedef  unsigned int uint32; 

/* Count is chosen to run main loop about 4 minutes */
static const int kLOOPCOUNT = 120 * 1000000;

/* Size is chosen to fit into a little less thsan 4KB */
static const int kSIZE = 960;	/* 4-byte words */


/* Return time of day in usec */
uint64 gettime() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (tv.tv_sec * 1000000L) + tv.tv_usec;
}

#define mix(a,b,c) \
{ \
  a -= b; a -= c; a ^= (c>>13); \
  b -= c; b -= a; b ^= (a<<8); \
  c -= a; c -= b; c ^= (b>>13); \
  a -= b; a -= c; a ^= (c>>12);  \
  b -= c; b -= a; b ^= (a<<16); \
  c -= a; c -= b; c ^= (b>>5); \
  a -= b; a -= c; a ^= (c>>3);  \
  b -= c; b -= a; b ^= (a<<10); \
  c -= a; c -= b; c ^= (b>>15); \
}

/* Calculate a hash over s, some multiple of 12 bytes long */
/* Length is count of 32-bit words */
uint32 hash(uint32* s, uint32 length, uint32 initval) {
   uint32 a,b,c,len;

   /* Set up the internal state */
   len = length;
   a = b = 0x9e3779b9;  /* the golden ratio; an arbitrary value */
   c = initval;         /* the previous hash value */

   /*---------------------------------------- handle most of the string */
   while (len >= 3)
   {
      a += s[0] ;
      b += s[1];
      c += s[2];
      mix(a,b,c);
      s += 3; len -= 3;
   }
   /*-------------------------------------------- report the result */
   return c;
}


/* Set up to run for about 4-5 minutes */
int main (int argc, const char** argv) {
  /* Simple arbitrary initialization */
  uint32 foo[kSIZE];	/* A little less than 4KB */
  for (int i = 0; i < kSIZE; ++i) {foo[i] = i * 1041667;}

  /* Main loop */
  uint32 hashval = 0;
  uint64 start = gettime();
  for (int i = 0; i < kLOOPCOUNT; ++i) {
    hashval = hash(foo, kSIZE, hashval);
  }
  uint64 elapsed = gettime() - start;

  /* Make sure hashval is live */
  fprintf(stdout, "elapsed usec %ld, hashval = %08X\n", elapsed, hashval);
  return 0;
}


