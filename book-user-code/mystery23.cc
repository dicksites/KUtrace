// schedtest.cc 
// Little program to observe scheduler choices
// Copyright 2021 Richard L. Sites
//
// compile with g++ -O2 -pthread mystery23.cc  kutrace_lib.cc  -o mystery23 

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "basetypes.h"
#include "kutrace_lib.h"

// From Jenkins hash
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


enum SchedType {
  CFS = 0,
  FIFO,
  RR
};

/* Count is chosen to run main loop about 1 second */
static const int kLOOPCOUNT = 8000;

/* Size is chosen to fit into a little less thsan 256KB */
static const int kSIZE = 64 * 960;	/* 4-byte words */

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


/* Do some work for about a second */
/* Return hashval to make it live; caller ignores */
void* CalcLoop(void* unused_arg) {
  //fprintf(stdout, "  CalcLoop(%d)\n", *(int*)unused_arg);

  /* Simple arbitrary initialization */
  uint32 foo[kSIZE];	/* A little less than 256KB */
  for (int i = 0; i < kSIZE; ++i) {foo[i] = (i & 1023) * 1041667;}

  /* Main loop */
  volatile uint32 hashval = 0;
  for (int i = 0; i < kLOOPCOUNT; ++i) {
    hashval = hash(foo, kSIZE, hashval);
  }

  return NULL;
}


void DoParallel(int n, SchedType schedtype) {
  kutrace::mark_d(n);
  //fprintf(stdout, "DoParallel(%d)\n", n);
  pthread_t* thread_id = (pthread_t*)malloc(n * sizeof(pthread_t)); 
  /* Spawn n threads */
  for (int i = 0; i < n; ++i) {
    pthread_attr_t attr;
    struct sched_param sparam;
    sparam.sched_priority = 1;
    pthread_attr_init(&attr);
    /* Defaults to CFS, called SCHED_OTHER */
    if (schedtype == FIFO) {
      pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
      pthread_attr_setschedparam(&attr, &sparam);
      pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    }
    if (schedtype == RR) {
      pthread_attr_setschedpolicy(&attr, SCHED_RR);
      pthread_attr_setschedparam(&attr, &sparam);
      pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    }
 
    int iret = pthread_create(&thread_id[i], NULL, CalcLoop, &i);
    if (iret != 0) {fprintf(stderr, "pthread_create() error %d\n", iret);}

    pthread_attr_destroy(&attr);
  }

  /* Wait for all n threads to finish */
  for (int i = 0; i < n; ++i) {
    pthread_join(thread_id[i], NULL);
    //fprintf(stdout, "  ret[%d]\n", i);
  }

  free(thread_id);
  //fprintf(stdout, "\n");

};

void Usage() {
  fprintf(stderr, "Usage: schedtest [-cfs(d) | -fifo | -rr]\n");
  exit(EXIT_FAILURE);
}


// Spawn different numbers of parallel threads
int main(int argc, const char** argv) {
  SchedType schedtype = CFS;	// default
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "-cfs") == 0) {schedtype = CFS;}
    if (strcmp(argv[i], "-fifo") == 0) {schedtype = FIFO;}
    if (strcmp(argv[i], "-rr") == 0) {schedtype = RR;}
  }

  // Spawn 1..12 parallel processes
  for (int n = 1; n <= 12; ++n) {
    DoParallel(n, schedtype);
  }

  exit(EXIT_SUCCESS);
}

