// Little program to dump KUtrace raw trace files in hex
// dsites 2022.05.27
//
// Usage: kuod filename
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned char uint8;
typedef unsigned long long int uint64;

void usage(void) {
  fprintf(stderr, "Usage: kuod <tracefile.trace\n");
  exit(0);
}

// Printable Ascii or dot
uint8 make_printable(uint8 c) {
  if (c < ' ') {return '.';}
  if ('~' < c) {return '.';}
  return c;
}

int main(int argc, const char** argv) {
  FILE* f;
  if (argc < 2) {
    f = stdin;
  } else {
    f = fopen(argv[1], "rb");
    if (f == NULL) {
      fprintf(stderr, "%s did not open\n", argv[1]);
      exit(0);
    }
    fprintf(stdout, "%s\n\n", argv[1]);
  }
    
  int n;
  uint64 buffer[1024];	// read 8KB at a time
  
  int offset = 0;
  bool skipping = false;
  while ((n = fread(buffer, 1, sizeof(buffer), f)) != 0) {
    int lenu64 = n >> 3;
    // Do four words per line (32 bytes)
    for (int i = 0; i < lenu64; i += 4) {
      offset += 32;
      
      // Skip lines of zeros
      if ((buffer[i + 0] == 0) && (buffer[i + 1] == 0) && 
          (buffer[i + 2] == 0) && (buffer[i + 3] == 0)) {
        if (!skipping) {fprintf(stdout, "  ...\n\n");}
        skipping = true;
        continue;
      } else {
        skipping = false;
      }

      // Print nonzero line
      fprintf(stdout, "[%06x] ", offset - 32);
      for (int j = 0; j < 4; ++j) {
        fprintf(stdout, "%05llx.%011llx ", 
          buffer[i + j] >> 44, buffer[i + j] & 0x00000FFFFFFFFFFFLL);
      }
      fprintf(stdout, "  ");
      for (int j = 0; j < 4; ++j) {
        uint8* cbuf = (uint8*)&buffer[i + j];
        for (int k = 0; k < 8; ++k) {
          fprintf(stdout, "%c", make_printable(cbuf[k]));
        }
        fprintf(stdout, " ");
      }
      fprintf(stdout, "\n");
    }
    if (!skipping) {fprintf(stdout, "\n");}
  }
  
  fclose(f);
  return 0;
}