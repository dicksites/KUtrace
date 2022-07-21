#include <string.h>
#include "basetypes.h"

static const char kFromBase40[40] = {
  '\0','a','b','c', 'd','e','f','g',  'h','i','j','k',  'l','m','n','o',
  'p','q','r','s',  't','u','v','w',  'x','y','z','0',  '1','2','3','4',
  '5','6','7','8',  '9','-','.','/', 
};

// Unpack six characters from 32 bits.
// str must be 8 bytes. We somewhat-arbitrarily capitalize the first letter
char* Base40ToChar(uint64 base40, char* str) {
  base40 &= 0x00000000fffffffflu;	// Just low 32 bits
  memset(str, 0, 8);
  // First character went in last, comes out first
  int i = 0;
  while (base40 > 0) {
    uint64 n40 = base40 % 40;
    str[i] = kFromBase40[n40];
    base40 /= 40;
    ++i;
  }
  return str;
}

