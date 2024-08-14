// Compile with g++ -O2 hello_world_trace.c kutrace_lib.cc -o hello_world_trace

#include <stdio.h>

#include "kutrace_lib.h"

int main (int argc, const char** argv) {
  kutrace::mark_a("hello");
  fprintf(stdout, "hello world\n");
  kutrace::mark_a("/hello");
  return 0;
}
