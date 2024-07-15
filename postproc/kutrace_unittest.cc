// Unit test for kutrace_lib
// dsites 2017.08.25
//
// Compile with g++ -O2 kutrace_unittest.cc kutrace_lib.cc -o kutrace_unittest
//

#include <stdio.h>
#include <string.h>

#include "kutrace_lib.h"


int main (int argc, const char** argv) {
  //Exit immediately if the module is not loaded
  if (!kutrace::test()) {
    fprintf(stderr, "FAIL, module kutrace_mod.ko not loaded\n");
    return 0;
  }

  // Executable image name, backscan for slash if any
  const char* slash = strrchr(argv[0], '/');
  kutrace::go((slash == NULL) ? argv[0] : slash + 1);
  kutrace::mark_a("write");
  kutrace::mark_b("/write");
  kutrace::mark_c("a");
  kutrace::mark_d(666);
  fprintf(stderr, "PASS, ./postproc3.sh /tmp/unittest.trace \"unittest\"\n");
  fprintf(stderr, "      ./kuod /tmp/unittest.trace\n");
  kutrace::stop("/tmp/unittest.trace");
  return 0;
}
