// Little program to paste in kernel names for PC addresses
// Copyright 2021 Richard L. Sites
// 
// Filter from stdin to stdout
// One command-line parameter -- allsyms file name 
//   $ cat foo.json |./samptoname_k >foo_with_k_pc.json
//
//
// Compile with g++ -O2 samptoname_k.cc -o samptoname_k
//
// Input from stdin is a KUtrace json file, some of whose events are
// PC samples of kernel addresses. We want to rewrite these with the
// corresponding routine name, taken from the second input.
//
//    ts           dur       cpu  pid  rpc event arg ret  name--------------------> 
//  [  0.00000000, 0.00400049, -1, -1, 33588, 641, 61259, 0, 0, "PC=ffffffffb43bd2e7"]
//
// Second input from filename is from 
// sudo cat /proc/kallsyms |sort >somefile.txt
//
//  ffffffffb43bd2a0 T clear_page_orig
//  ffffffffb43bd2e0 T clear_page_erms
//  ffffffffb43bd2f0 T cmdline_find_option_bool
//  ffffffffb43bd410 T cmdline_find_option
//
// Output to stdout is the input json with names substituted and the
// hash code in arg updated
//  [  0.00000000, 0.00400049, -1, -1, 33588, 641, 12345, 0, 0, "PC=clear_page_erms"]
//


#include <map>
#include <string>

#include <stdio.h>
#include <stdlib.h>     // exit
#include <string.h>

#include "basetypes.h"
#include "kutrace_lib.h"

using std::string;
using std::map;

typedef struct {
  double start_ts;	// Seconds
  double duration;	// Seconds
  int64 start_ts_ns;
  int64 duration_ns;
  int cpu;
  int pid;
  int rpcid;
  int eventnum;
  int arg;
  int retval;
  int ipc;
  string name;
} OneSpan;

typedef map<uint64, string> SymMap;

// Add dummy entry that sorts last, then close the events array and top-level json
void FinalJson(FILE* f) {
  fprintf(f, "[999.0, 0.0, 0, 0, 0, 0, 0, 0, 0, \"\"]\n");	// no comma
  fprintf(f, "]}\n");
}

static const int kMaxBufferSize = 256;

// Read next line, stripping any crlf. Return false if no more.
bool ReadLine(FILE* f, char* buffer, int maxsize) {
  char* s = fgets(buffer, maxsize, f);
  if (s == NULL) {return false;}
  int len = strlen(s);
  // Strip any crlf or cr or lf
  if (s[len - 1] == '\n') {s[--len] = '\0';}
  if (s[len - 1] == '\r') {s[--len] = '\0';}
  return true;
}


void ReadAllsyms(FILE* f, SymMap* allsyms) {
  uint64 addr = 0LL;
  char buffer[kMaxBufferSize];
  while (ReadLine(f, buffer, kMaxBufferSize)) {
    size_t len = strlen(buffer);
    size_t space1 = strcspn(buffer, " \t");
    if (len <= space1) {continue;}
    buffer[space1] = '\0';

    size_t space2 = space1 + 1 + strcspn(buffer + space1 + 1, " \t");
    if (len <= space2) {continue;}
    buffer[space2] = '\0';

    size_t space3 = space2 + 1 + strcspn(buffer + space2 + 1, " \t");
    // Space3 is optional
    buffer[space3] = '\0';

    int n = sscanf(buffer, "%llx", &addr);
    if (n != 1) {continue;}
    string name = string(buffer + space2 + 1);
    (*allsyms)[addr] = name;
//fprintf(stdout, "allsyms[%llx] = %s\n", addr, name.c_str());
  }
  // We don't know how far the last item extends.
  // Arbitrarily assume that it is 4KB and add a dummy entry at that end
  if (addr < 0xffffffffffffffffL - 4096L) {
    (*allsyms)[addr + 4096] = string("-dummy-");
  }
}

string Lookup(const string& s, const SymMap& allsyms) {
  if (s.find_first_not_of("0123456789abcdef") != string::npos) {
    // Not valid hex. Leave unchanged.
//fprintf(stdout, "Lookup(%s) unchanged\n", s.c_str());
    return string("");
  }
  uint64 addr = 0;
  sscanf(s.c_str(), "%llx", &addr);
  SymMap::const_iterator it = allsyms.upper_bound(addr);  // Just above addr
  it = prev(it);	// At or just below addr
//fprintf(stdout, "Lookup(%s %llx) = %s\n", s.c_str(), addr, it->second.c_str());
  return it->second;
}

// Cheap 16-bit hash so we can mostly distinguish different routine names
int NameHash(const string& s) {
  uint64 hash = 0L;
  for (int i = 0; i < s.length(); ++i) {
    uint8 c = s[i];		// Make sure it is unsigned
    hash = (hash << 3) ^ c;	// ignores leading chars if 21 < len
  }
  hash ^= (hash >> 32);	// Fold down
  hash ^= (hash >> 16);
  int retval = static_cast<int>(hash & 0xffffL);
  return retval;
}



// Input is a json file of spans
// start time and duration for each span are in seconds
// Output is a smaller json file of fewer spans with lower-resolution times
void Usage() {
  fprintf(stderr, "Usage: spantopcnamek <allsyms fname>\n");
  exit(0);
}

//
// Filter from stdin to stdout
//
int main (int argc, const char** argv) {
  if (argc < 2) {Usage();}

  // Input allsyms file
  SymMap allsyms;

  const char* fname = argv[1];
  FILE* f = fopen(fname, "r");
  if (f == NULL) {
    fprintf(stderr, "%s did not open\n", fname);
    exit(0);
  }
  ReadAllsyms(f, &allsyms);
  fclose(f);
  
  
  // expecting:
  //    ts           dur       cpu  pid  rpc event arg ret  name--------------------> 
  //  [  0.00000000, 0.00400049, -1, -1, 33588, 641, 61259, 0, 0, "PC=ffffffffb43bd2e7"],

  int output_events = 0;
  char buffer[kMaxBufferSize];
  while (ReadLine(stdin, buffer, kMaxBufferSize)) {
    char buffer2[256];
    buffer2[0] = '\0';
    OneSpan onespan;
    int n = sscanf(buffer, "[%lf, %lf, %d, %d, %d, %d, %d, %d, %d, %s",
                   &onespan.start_ts, &onespan.duration, 
                   &onespan.cpu, &onespan.pid, &onespan.rpcid, 
                   &onespan.eventnum, &onespan.arg, &onespan.retval, &onespan.ipc, buffer2);
    onespan.name = string(buffer2);
    // fprintf(stderr, "%d: %s\n", n, buffer);
    
    if (n < 10) {
      // Copy unchanged anything not a span
      fprintf(stdout, "%s\n", buffer);
      continue;
    }
    if (onespan.start_ts >= 999.0) {break;}	// Always strip 999.0 end marker and stop

    if (onespan.eventnum == KUTRACE_PC_K) {
      string oldname = onespan.name.substr(4);	// Skip over "PC=
      size_t quote2 = oldname.find("\"");
      if (quote2 != string::npos) {oldname = oldname.substr(0, quote2);}
      string newname = Lookup(oldname, allsyms);
      if (!newname.empty()) {
        onespan.name = "\"PC=" + newname + "\"],";
        onespan.arg = NameHash(newname);
      }
    }

#if 1
    // Name has trailing punctuation, including ],
    fprintf(stdout, "[%12.8f, %10.8f, %d, %d, %d, %d, %d, %d, %d, %s\n",
            onespan.start_ts, onespan.duration,
            onespan.cpu, onespan.pid, onespan.rpcid, onespan.eventnum, 
            onespan.arg, onespan.retval, onespan.ipc, onespan.name.c_str());
    ++output_events;
#endif
  }

  // Add marker and closing at the end
  FinalJson(stdout);
  fprintf(stderr, "spantopcnamek: %d events\n", output_events);

  return 0;
}
