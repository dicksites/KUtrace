/* Lots of floating double divides */
/* Chosen only because it fills up time with few issue slots and without much memory activity */
// Copyright 2021 Richard L. Sites

#include <stdio.h>
#include <sys/time.h>	/* gettimeofday */

typedef  unsigned long int uint64;
typedef  unsigned int uint32; 

/* Return time of day in usec */
uint64 gettime() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (tv.tv_sec * 1000000L) + tv.tv_usec;
}


/* Calculate bogus work */
/* Length is count of 32-bit words */
double boguscalc(double initval) {
  double d = initval;
  for (int i = 0; i < 1000; ++i) {
    d /= 1.000000001;
    d /= 0.999999999;
  }
  return d;
}

/* Count is chosen to run main loop about 4 minutes */
static const int kLOOPCOUNT = 35 * 1000000;

/* Set up to run for about 4-5 minutes */
int main (int argc, const char** argv) {
  /* Simple arbitrary initialization */
  double foo = 123456789.0;

  /* Main loop */
  uint64 start = gettime();
  for (int i = 0; i < kLOOPCOUNT; ++i) {
    foo = boguscalc(foo);
  }
  uint64 elapsed = gettime() - start;

  fprintf(stdout, "elapsed usec %ld, foo = %18.17f\n", elapsed, foo);
  return 0;
}


