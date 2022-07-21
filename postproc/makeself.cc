// Little program to make a self-contained HTML file for displaying dclab graphs.
// dick sites 2017.09.29
// dick sites 2017.12.07 Allows pipe from stdin
// dick sites 2020.06.05 Explicitly check for sorted input
// dsites 20201.01.07 Only check for sorted until end of events[]. More unsorted may be added after that.
//
// Inputs
// (1) A base HTML file with everything except for a library and json data
//     This file contains three stylized comments that indicate where to 
//     include the other pieces, specified as arg[1]
// (2) The d3.v4.min.js JavaScript library, fetched from the same directory as this program
// (3) A JSON file of data to graph, specified as arg[2]
//
// Output
//     A new self-contained HTML file written to arg[3]
//

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>		// exit
#include <string.h>

static const char* const_text_1 = "<script>";
static const char* const_text_2 = "</script>";

static const char* const_text_3 = "var myString = '";
static const char* const_text_4 = "';";

//static const char* const_text_5 = "data = JSON.parse(myString); newdata2_resize(data);";
// Now uses onload="initAll()"
static const char* const_text_5 = "";
static const char* const_text_6 = "";


void usage() {
  fprintf(stderr, "Usage: makeself <input html> <input json> <output html>\n");
  exit(0);
}

int main (int argc, const char** argv) {
  if (argc < 2) {usage();}

  FILE* finlib = fopen("d3.v4.min.js", "rb");
  if (finlib == NULL) {fprintf(stderr, "%s did not open.\n", "d3.v4.min.js");}

  FILE* finhtml = fopen(argv[1], "rb");
  if (finhtml == NULL) {fprintf(stderr, "%s did not open.\n", argv[1]);}

  FILE* finjson = NULL;
  FILE* fouthtml = NULL;
  if (argc >= 4) {
    finjson = fopen(argv[2], "rb");
    if (finjson == NULL) {fprintf(stderr, "%s did not open.\n", argv[2]);}

    fouthtml = fopen(argv[3], "wb");
    if (fouthtml == NULL) {fprintf(stderr, "%s did not open.\n", argv[3]);}
  } else if (argc == 3) {
    // Pipe from stdin 
    finjson = stdin;

    fouthtml = fopen(argv[2], "wb");
    if (fouthtml == NULL) {fprintf(stderr, "%s did not open.\n", argv[2]);}
  } else {
    // Pipe from stdin and to stdout 
    finjson = stdin;
    fouthtml = stdout;
  }

  if (finhtml == NULL || finjson == NULL || finlib == NULL || fouthtml == NULL) {
    exit(0);
  }

  char* inlib_buf =  new char[  1000000];
  char* inhtml_buf = new char[  1000000];
  char* injson_buf = new char[250000000];	// 250MB

  int lib_len = fread(inlib_buf, 1, 1000000, finlib);
  fclose(finlib);

  int html_len = fread(inhtml_buf, 1, 1000000, finhtml);
  fclose(finhtml);

  int json_len = fread(injson_buf, 1, 250000000, finjson);
  if (finjson != stdin) {fclose(finjson);}

  char* self0 = strstr(inhtml_buf, "<!-- selfcontained0 -->");
  char* self1 = strstr(inhtml_buf, "<!-- selfcontained1 -->");
  char* self2 = strstr(inhtml_buf, "<!-- selfcontained2 -->");

  if (self0 == NULL || self1 == NULL || self2 == NULL) {
    fprintf(stderr, "%s does not contain selfcontained* comments\n", argv[1]);
    exit(0);
  }

  char* self0_end = strchr(self0, '\n');
  if (self0_end == NULL) {fprintf(stderr, "Missing <cr> after selfcontained0\n");}
  ++self0_end;	// over the <cr>

  char* self0_cr2 = strchr(self0_end, '\n');
  if (self0_cr2 == NULL) {fprintf(stderr, "Missing second <cr> after selfcontained0\n");}
  ++self0_cr2;	// over the <cr>

  char* self1_end = strchr(self1 + 1, '\n');
  if (self1_end == NULL) {fprintf(stderr, "Missing <cr> after selfcontained1\n");}
  ++self1_end;	// over the <cr>

  char* self2_end = strchr(self2 + 1, '\n');
  if (self2_end == NULL) {fprintf(stderr, "Missing <cr> after selfcontained2\n");}
  ++self2_end;	// over the <cr>


  // Output is 
  //  inhtml_buf up to self0 (len1),
  //  minus the next line (the include for d3.v4.min.js)
  //  plus constant text
  //  plus inlib_buf
  //  plus constant text
  //
  //  plus inhtml_buf between self0 and self1 (len2)
  //  plus constant text
  //  plus injson_buf with all <cr> turned into space
  //  plus constant text
  //
  //  plus inhtml_buf between self1 and self2 (len3)
  //  plus constant text to display json
  //  plus constant text
  //
  //  plus inhtml_buf after self2 (len4)

  const char* prior_line = &injson_buf[0];
  int linenum = 1;
  bool check_sorted = true;
  for (int i = 0; i < json_len; ++i) {
    if (injson_buf[i] == '\n') {
      ++linenum;
      const char* next_line = &injson_buf[i + 1];
      // Check for sorted
      if (i < json_len - 5) {
        if (check_sorted && (strncmp(prior_line, next_line, 4) > 0)) {
          fprintf(stderr, "Input not sorted at line %d\n", linenum);
          char temp[64];
          strncpy(temp, next_line, 64);
          temp[63] = '\0';
          fprintf(stderr, "  '%s...'\n", temp);
          exit(0);
        }
        // Stop checking sorted at first line that has "[999.0," in column 1
        if (strncmp(next_line, "[999", 4) == 0) {check_sorted = false;}
        // Stop checking sorted if line has " \"unsorted\"" in column 1
        // Note leading space.
        if ((i < json_len - 11) && (strncmp(next_line, " \"unsorted\"", 11) == 0)) {check_sorted = false;}
        // Stop checking sorted if line has " \"presorted\"" in column 1
        if ((i < json_len - 12) && (strncmp(next_line, " \"presorted\"", 12) == 0)) {check_sorted = false;}
      }

      prior_line = next_line;
      // Replace newline with space -- JSON string may not contain newline
      injson_buf[i] = ' ';
      // Replace backslash with two of them
      // Replace quote with backslash quote
    } 
  }

  // Lengths of four inhtml pieces
  int len1 = self0_end - inhtml_buf;
  int len2 = self1_end - self0_cr2;	// Skips one line of d3.v4.min.js include
  int len3 = self2_end - self1_end;
  int len4 = (inhtml_buf + html_len) - self2_end;

  fwrite(inhtml_buf, 1, len1, fouthtml);
  fwrite(const_text_1, 1, strlen(const_text_1), fouthtml);
  fwrite(inlib_buf, 1, lib_len, fouthtml);
  fwrite(const_text_2, 1, strlen(const_text_2), fouthtml);

  fwrite(self0_cr2, 1, len2, fouthtml);

  fwrite(const_text_3, 1, strlen(const_text_3), fouthtml);
  fwrite(injson_buf, 1, json_len, fouthtml);
  fwrite(const_text_4, 1, strlen(const_text_4), fouthtml);

  fwrite(self1_end, 1, len3, fouthtml);
  fwrite(const_text_5, 1, strlen(const_text_5), fouthtml);
  fwrite(const_text_6, 1, strlen(const_text_6), fouthtml);

  fwrite(self2_end, 1, len4, fouthtml);
  if (fouthtml != stdout) {fclose(fouthtml);}  

  free(inlib_buf);
  free(inhtml_buf);
  free(injson_buf);
  return 0;
}

