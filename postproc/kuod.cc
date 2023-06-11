// Little program to dump KUtrace raw trace files in hex
// dsites 2022.05.27
//
// Usage: kuod filename <-all> <-t>
//

// dsites 2023.04.14 add showing local datetime for each raw block header

// compile with g++ -O2 kuod.cc -o kuod

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "kutrace_lib.h"

typedef unsigned char uint8;
typedef unsigned long long int uint64;

bool printall = false;

void usage(void) {
  fprintf(stderr, "Usage: kuod <tracefile.trace> <-all>\n");
  exit(0);
}

// Printable Ascii or dot
uint8 make_printable(uint8 c) {
  if (c < ' ') {return '.';}
  if ('~' < c) {return '.';}
  return c;
}

bool IsName(uint64 event) {
  int event_num = (event >> 32) & 0xFFF;
  if (event_num == KUTRACE_PC_U) {return true;}
  if (event_num == KUTRACE_PC_K) {return true;}
  return ((KUTRACE_VARLENLO <= event_num) && (event_num <= KUTRACE_VARLENHI));
}

int NameLen(uint64 event) {
  int event_num = (event >> 32) & 0xFFF;
  // Special-case hisgtorical accident with missing len=2
  if (event_num == KUTRACE_PC_TEMP) {return 2;}
  if (event_num == KUTRACE_PC_U) {return 2;}
  if (event_num == KUTRACE_PC_K) {return 2;}
  return (event_num >> 4) & 0x00F;
}

bool IsHeaderWord(bool has_ipc, int block_8k, int wordnum) {
  // Very first block has 12-word header
  if ((block_8k == 0) && (wordnum < 12)) {return true;}
  // Other headers start on multiples of 8 (no IPC) or 9 (IPC) chunks of 8KB 
  if (has_ipc) {
    if (((block_8k % 9) == 0) && (wordnum < 6)) {return true;}
  } else {
    if (((block_8k % 8) == 0) && (wordnum < 6)) {return true;}
  }
  return false;
}

// Return true if this 8KB is the beginning of a 64KB/72KB trace block
bool IsBlockHeader(bool has_ipc, int block_8k) {
  if (has_ipc) {
    if ((block_8k % 9) == 0) {return true;}
  } else {
    if ((block_8k % 8) == 0) {return true;}
  }
  return false;
}

bool IsIpcWord(bool has_ipc, int block_8k, int wordnum) {
  // If IPC, the values are in every 9th 8K block
  if (!has_ipc) {return false;}
  if ((block_8k % 9) == 8) {return true;}
  return false;
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
  
  // If any extra parameter, treat as print all lines of zero
  if (3 <= argc) {printall = true;}
    
  int n;
  uint64 buffer[1024];	// read 8KB at a time
  
  size_t offset = 0;
  bool skipping = false;
  int inside_name = 0;
  int block_8k = 0;
  bool has_ipc = false;
  while ((n = fread(buffer, 1, sizeof(buffer), f)) != 0) {
    int lenu64 = n >> 3;
    if (block_8k == 0) {
      has_ipc = (((buffer[1] >> 56) & 0x80) != 0);  // High flag bit is IPC bit
    }

    // Show datetime for each block header
    if (IsBlockHeader(has_ipc, block_8k)) {
      uint64 block_start_usec = buffer[1] & 0x00FFFFFFFFFFFFFFLL; 
      time_t block_start_sec = block_start_usec / 1000000;
      char* block_start_ctime = ctime(&block_start_sec);
      // String extraneous trailing \n and also strip date
      block_start_ctime[strlen(block_start_ctime) - 6] = '\0';
      fprintf(stdout, "\n%s.%06llu block[%04d]\n",
        block_start_ctime, block_start_usec % 1000000, block_8k / (8 + has_ipc));
    }

    // Do four words per line (32 bytes)
    for (int i = 0; i < lenu64; i += 4) {
      offset += 32;
      
      // Skip lines of zeros
      if (!printall &&
          (buffer[i + 0] == 0) && (buffer[i + 1] == 0) && 
          (buffer[i + 2] == 0) && (buffer[i + 3] == 0)) {
        if (!skipping) {fprintf(stdout, "  ...\n\n");}
        skipping = true;
        inside_name = 0;
        continue;
      } else {
        skipping = false;
      }

      // Print nonzero line
      fprintf(stdout, "[%06lx] ", offset - 32);
      for (int j = 0; j < 4; ++j) {
        if (inside_name > 0) {
          fprintf(stdout, "_%016llx ", buffer[i + j]);
          --inside_name;
        } else { 
          if (IsHeaderWord(has_ipc, block_8k, i + j) || 
              IsIpcWord(has_ipc, block_8k, i + j)) {
            // First 6 (12 in first block) words are header: don't punctuate
            fprintf(stdout, "%016llx  ", buffer[i + j]);
          } else {
            // Normal word; dot after timestamp
            fprintf(stdout, "%05llx.%011llx ", 
              buffer[i + j] >> 44, buffer[i + j] & 0x00000FFFFFFFFFFFLL);
            if (IsName(buffer[i + j])) {
              inside_name = NameLen(buffer[i + j]) - 1;
            }
          }
        }
      }	// End for j
      fprintf(stdout, "  ");
      for (int j = 0; j < 4; ++j) {
        uint8* cbuf = (uint8*)&buffer[i + j];
        for (int k = 0; k < 8; ++k) {
          fprintf(stdout, "%c", make_printable(cbuf[k]));
        }
        fprintf(stdout, " ");
      }
      fprintf(stdout, "\n");
    }	// End for i
    if (!skipping) {fprintf(stdout, "\n");}
    ++block_8k;
  }
  
  fclose(f);
  return 0;
}
