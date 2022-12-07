#include <stdatomic.h>
typedef unsigned long long u64;
typedef atomic_ullong atomic64_t;

int main (int argc, const char** argv) {
  long long unsigned int a;
  u64 b;
  u64 r;

/*  r = ((u64)atomic_add_return(a*8,(atomic64_t*)&b)) - a; */
  r = ((u64)atomic_fetch_add((atomic64_t*)&b, a*8));
  return 0;
}

