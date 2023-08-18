// Little program to turn input strings into base40 values
// Copyright 2021 Richard L. Sites
//
// compile with g++ -O2 base40.cc -o base40
//

// Example output:
#define BASE40_a 1	// "a"
#define BASE40__a 79	// "/a"
#define BASE40_cow 37403	// "cow"
#define BASE40__cow 1496159	// "/cow"
#define BASE40_zero 989026	// "zero"
#define BASE40__zero 39561079	// "/zero"


#include <iostream>
#include <stdio.h>
#include <string.h>

typedef unsigned long int u64;

// Uppercase mapped to lowercase
// All unexpected characters mapped to '-'
//   - = 0x2D . = 0x2E / = 0x2F
// Base40 characters are _abcdefghijklmnopqrstuvwxyz0123456789-./
//                       0         1         2         3
//                       0123456789012345678901234567890123456789
// where the first is NUL.
static const char kToBase40[256] = {
   0,38,38,38, 38,38,38,38, 38,38,38,38, 38,38,38,38,
  38,38,38,38, 38,38,38,38, 38,38,38,38, 38,38,38,38,
  38,38,38,38, 38,38,38,38, 38,38,38,38, 38,37,38,39,
  27,28,29,30, 31,32,33,34, 35,36,38,38, 38,38,38,38,

  38, 1, 2, 3,  4, 5, 6, 7,  8, 9,10,11, 12,13,14,15,
  16,17,18,19, 20,21,22,23, 24,25,26,38, 38,38,38,38,
  38, 1, 2, 3,  4, 5, 6, 7,  8, 9,10,11, 12,13,14,15,
  16,17,18,19, 20,21,22,23, 24,25,26,38, 38,38,38,38,

  38,38,38,38, 38,38,38,38, 38,38,38,38, 38,38,38,38,
  38,38,38,38, 38,38,38,38, 38,38,38,38, 38,38,38,38,
  38,38,38,38, 38,38,38,38, 38,38,38,38, 38,38,38,38,
  38,38,38,38, 38,38,38,38, 38,38,38,38, 38,38,38,38,

  38,38,38,38, 38,38,38,38, 38,38,38,38, 38,38,38,38,
  38,38,38,38, 38,38,38,38, 38,38,38,38, 38,38,38,38,
  38,38,38,38, 38,38,38,38, 38,38,38,38, 38,38,38,38,
  38,38,38,38, 38,38,38,38, 38,38,38,38, 38,38,38,38,
};

static const char kFromBase40[40] = {
  '\0','a','b','c', 'd','e','f','g',  'h','i','j','k',  'l','m','n','o',
  'p','q','r','s',  't','u','v','w',  'x','y','z','0',  '1','2','3','4',
  '5','6','7','8',  '9','-','.','/',
};

// Unpack six characters from 32 bits.
// str must be 8 bytes. We somewhat-arbitrarily capitalize the first letter
char* Base40ToChar(u64 base40, char* str) {
  base40 &= 0x00000000fffffffflu;	// Just low 32 bits
  memset(str, 0, 8);
  bool first_letter = true;
  // First character went in last, comes out first
  int i = 0;
  while (base40 > 0) {
    u64 n40 = base40 % 40;
    str[i] = kFromBase40[n40];
    base40 /= 40;
    if (first_letter && (1 <= n40) && (n40 <= 26)) {
      str[i] &= ~0x20; 		// Uppercase it
      first_letter = false;
    }
    ++i;
  }
  return str;
}

// Pack six characters into 32 bits. Only use a-zA-Z0-9.-/
u64 CharToBase40(const char* str) {
  int len = strlen(str);
  // If longer than 6 characters, take only the first 6
  if (len > 6) {len = 6;}
  u64 base40 = 0;
  // First character goes in last, comes out first
  for (int i = len - 1; i >= 0; -- i) {
    base40 = (base40 * 40) + kToBase40[str[i]];
  }
  return base40;
}



static const int kBuffersize = 128;
// Allowed in C variable names; others replaced with underscore
static const char* kAllowed = "_ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";

int main (int argc, const char** argv) {
  char buffer2[kBuffersize];
  std::string label, closing_label;
  while (std::cin >> label) {
    closing_label = "/" + label;
    for (int i = 0; i < label.length(); i++) {
      buffer2[i] = (strchr(kAllowed, label[i]) != NULL) ? label[i] : '_';
    }
    buffer2[label.length()] = '\0';
    //printf("%ld // %s\n", CharToBase40(buffer), buffer);
    printf("#define BASE40_%s  %ld    // \"%s\"\n", buffer2, CharToBase40(label.c_str()), label.c_str());
    printf("#define BASE40__%s %ld  // \"%s\"\n", buffer2, CharToBase40(closing_label.c_str()), closing_label.c_str());
  }
  return 0;
}

