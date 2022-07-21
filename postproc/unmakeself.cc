// Little program to UN-make a self-contained HTML file for displaying dclab graphs.
// Copyright 2021 Richard L. Sites
//
// Inputs
//     Self-contained HTML file 
//
// Output
//     The contained JSON file written to stdout
//     If you want, then pipe through sed 's/], /],\n/g'
//

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>		// exit
#include <string.h>

static const char* const_text_1 = "<script>";
static const char* const_text_2 = "</script>";

static const char* const_text_3 = "var myString = '";
static const char* const_text_4 = "';";

static const char* const_text_5 = "data = JSON.parse(myString); newdata2_resize(data);";
static const char* const_text_6 = "";


void usage() {
  fprintf(stderr, "Usage: unmakeself <input html>\n");
  exit(0);
}

int main (int argc, const char** argv) {
  FILE* finhtml;
  if (argc < 2) {
    finhtml = stdin;
  } else {
    finhtml = fopen(argv[1], "rb");
    if (finhtml == NULL) {
      fprintf(stderr, "%s did not open.\n", argv[1]);
      return 0;
    }
  }

  char* inhtml_buf = new char[250000000];	// 250MB

  int html_len = fread(inhtml_buf, 1, 250000000, finhtml);
  fclose(finhtml);


  char* self0 = strstr(inhtml_buf, "<!-- selfcontained0 -->");
  char* self1 = strstr(inhtml_buf, "<!-- selfcontained1 -->");
  char* self2 = strstr(inhtml_buf, "<!-- selfcontained2 -->");

  if (self0 == NULL || self1 == NULL || self2 == NULL) {
    fprintf(stderr, "%s does not contain selfcontained* comments\n", argv[1]);
    exit(0);
  }

  char* self1_end = strchr(self1 + 1, '\n');
  if (self1_end == NULL) {fprintf(stderr, "Missing <cr> after selfcontained1\n");}
  ++self1_end;	// over the <cr>

  char* self2_end = strchr(self2 + 1, '\n');
  if (self2_end == NULL) {fprintf(stderr, "Missing <cr> after selfcontained2\n");}
  ++self2_end;	// over the <cr>


  //for (int i = 0; i < json_len; ++i) {
  //  if (injson_buf[i] == '\n') {
  //    injson_buf[i] = ' ';
  //  } 
  //}

  // JSON is in self1_end .. self_2
  // Within this, there is a single-quote string that we want.
  *self2 = '\0';
  char* quote1 = strchr(self1_end, '\'');
  if (quote1 == NULL) {
    fprintf(stderr, "Missing '..' string\n");
    return 0;
  }
//fprintf(stderr, "quote1 at offset %d\n", (int)(quote1 - inhtml_buf));
  ++quote1;	// Over the quote

  char* quote2 = strchr(quote1, '\'');
  if (quote2 == NULL) {
    fprintf(stderr, "Missing '..' string\n");
    return 0;
  }
//fprintf(stderr, "quote2 at offset %d\n", (int)(quote2 - inhtml_buf));

  // Length of json inhtml piece
  int len3 = quote2 - quote1;
  fwrite(quote1, 1, len3, stdout);

  free(inhtml_buf);
  return 0;
}

