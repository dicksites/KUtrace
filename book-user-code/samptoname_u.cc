// Little program to paste in user names for PC addresses
// Copyright 2021 Richard L. Sites
// 
// Filter from stdin to stdout
// One command-line parameter -- pidmaps file name 
//
//
// TODO: 
//  cache lookup results to avoid excess command spawning
//
// Compile with g++ -O2 spantopcnameu.cc -o spantopcnameu
//
// Input from stdin is a KUtrace json file, some of whose events are
// PC samples of kernel addresses. We want to rewrite these with the
// corresponding routine name, taken from the second input.
//
//    ts           dur       cpu  pid  rpc event arg ret  name--------------------> 
//  [ 26.65163778, 0.00400013, 2, 10129, 37094, 640, 58156, 0, 0, "PC=7f31f1f8cb1f"],
//
// Second input from filename is from 
//   sudo ls /proc/*/maps |xargs -I % sh -c 'echo "\n====" %; sudo cat %' >somefile.txt
//
//  ==== /proc/10000/maps
//  5636c2def000-5636c2eac000 r-xp 00000000 08:02 5510282                    /usr/sbin/sshd
//  5636c30ab000-5636c30ae000 r--p 000bc000 08:02 5510282                    /usr/sbin/sshd
//  5636c30ae000-5636c30af000 rw-p 000bf000 08:02 5510282                    /usr/sbin/sshd
//  5636c30af000-5636c30b8000 rw-p 00000000 00:00 0 
//  5636c31be000-5636c31ee000 rw-p 00000000 00:00 0                          [heap]
//  7f0c2c372000-7f0c2c37c000 r-xp 00000000 08:02 5374723                    /lib/x86_64-linux-gnu/security...
//  address                  perms offset   dev   inode                      pathname
// see mmap(2)
//
// Note that we only care about the executable regions in the above: in r-xp the "x"
//
// Output to stdout is the input json with names substituted and the
// hash code in arg updated
//  [  0.00000000, 0.00400049, -1, -1, 33588, 641, 12345, 0, 0, "PC=memcpy-ssse3.S:1198"]
//


#include <map>
#include <string>

#include <stdio.h>
#include <stdlib.h>     // exit
#include <string.h>

#include "basetypes.h"
#include "kutrace_lib.h"

#define BUFFSIZE 256
#define CR 0x0d
#define LF 0x0a

static const int kMaxBufferSize = 256;

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

// We are going to pack a PID and the low address of a range into a single 64 bit key:
// top 16 bits for PID, low 48 bits for address. This could be changed later 
// to use a pair of uint64 or a single uint128 as key.

typedef struct {
  uint64 addr_lo;
  uint64 addr_hi;
  uint64 pid;
  string pathname;
} RangeToFile;

typedef map<uint64, RangeToFile> MapsMap;

// Add dummy entry that sorts last, then close the events array and top-level json
void FinalJson(FILE* f) {
  fprintf(f, "[999.0, 0.0, 0, 0, 0, 0, 0, 0, 0, \"\"]\n");	// no comma
  fprintf(f, "]}\n");
}

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

void DumpRangeToFile(FILE* f, const RangeToFile rtf) {
  fprintf(f, "%lld %llx %llx %s\n", rtf.pid, rtf.addr_lo, rtf.addr_hi, rtf.pathname.c_str()); 
}

//  5636c2def000-5636c2eac000 r-xp 00000000 08:02 5510282                    /usr/sbin/sshd
void ReadAllmaps(FILE* f, MapsMap* allmaps) {
  uint64 addr_lo = 0L;
  uint64 addr_hi = 0L;
  uint64 current_pid = 0L;
  char buffer[kMaxBufferSize];
  while (ReadLine(f, buffer, kMaxBufferSize)) {
    size_t len = strlen(buffer);
    if (memcmp(buffer, "==== /proc/", 11) == 0) {
       current_pid = atoi(&buffer[11]);
//fprintf(stdout, "pid %lld %s\n", current_pid, buffer);
       continue;
    }

    size_t space1 = strcspn(buffer, " \t");
    if (len <= space1) {continue;}
    buffer[space1] = '\0';

    size_t space2 = space1 + 1 + strcspn(buffer + space1 + 1, " \t");
    if (len <= space2) {continue;}
    buffer[space2] = '\0';

    size_t slash = space2 + 1 + strcspn(buffer + space2 + 1, "/");
    if (len <= slash) {continue;}

    int n = sscanf(buffer, "%llx-%llx", &addr_lo, &addr_hi);
    if (n != 2) {continue;}

    if (strchr(buffer + space1 + 1, 'x') ==NULL) {continue;}

    string pathname = string(buffer + slash);

    RangeToFile temp;
    temp.addr_lo = addr_lo;
    temp.addr_hi = addr_hi;
    temp.pid = current_pid;
    temp.pathname = pathname;
    uint64 key = (current_pid << 48) | (addr_lo & 0x0000FFFFFFFFFFFFL);

    (*allmaps)[key] = temp;
//fprintf(stdout, "allmaps[%016lx] = ", key);
//DumpRangeToFile(stdout, temp);
  }
}


// a must be the smaller
bool IsClose(uint32 a, uint32 b) {
  return  (b - a) < 12;			// An arbitrary limit of 12 consecutive PIDs
}

// Returns 0 if not valid hex
uint64 GetFromHex(const string& s) {
  if (s.find_first_not_of("0123456789abcdef") != string::npos) {
    return 0L;
  }
  uint64 addr = 0;
  sscanf(s.c_str(), "%llx", &addr);
  return addr;
}

const RangeToFile* Lookup(int pid, uint64 addr, const MapsMap& allmaps) {
  bool fail = false;
  if (pid < 0) {fail = true;}
  if (fail) {return NULL;}

  uint64 key = pid;
  key = (key << 48) | (addr & 0x0000FFFFFFFFFFFFL);
  MapsMap::const_iterator it = allmaps.upper_bound(key);  // Just above key
  it = prev(it);	// At or just below key
//fprintf(stdout, "Lookup(%d %llx %llx) = ", pid, addr, key);
//DumpRangeToFile(stdout, it->second);

  // If process P spawns processes Q R and S, most often they will have PIDs P+1 P+2 and P+3
  // and of course the same shared memory map. In this case, the lookup above will hit on the
  // entry just after the LAST entry for P before we call prev(). We can see that the PID for 
  // Q does not match the lookup result that is at the end of P, but we can see that Q's PID
  // of P+1 is "close". If that happens, we try looking up again using pID P in the key.

  if ((it->second.pid != pid) && (IsClose(it->second.pid, pid))) {
    // Second try with the lower PID
    uint64 maybeparentpid = it->second.pid; 
    key = (maybeparentpid << 48) | (addr & 0x0000FFFFFFFFFFFFL);
    it = allmaps.upper_bound(key);  // Just above key
    it = prev(it);	// At or just below key
//fprintf(stdout, "Lookup2(%lld %llx %llx) = ", maybeparentpid, addr, key);
//DumpRangeToFile(stdout, it->second);
    if (it->second.pid != maybeparentpid) {fail = true;}
    if (addr < it->second.addr_lo) {fail = true;}
    if (it->second.addr_hi <= addr) {fail = true;}
  } else {
    // Double-check that we had a real hit
    if (it->second.pid != pid) {fail = true;}
    if (addr < it->second.addr_lo) {fail = true;}
    if (it->second.addr_hi <= addr) {fail = true;}
  }

  if (fail) {
//fprintf(stdout, "  false hit\n");
    return NULL;
  }

  return &it->second;
}


char* NoArgs(char* procname) {
  char* paren = strchr(procname, '(');
  if (paren != NULL) {*paren = '\0';}
  return procname;
}

// Expecting two lines, the procedure name (from -f) and the file:line#
// If file:line# is unknown (not enough debug info), then it is ??:?
// The demangled (from -C) procedure name may have argument types.
// The file name does not have the full path (from -s)
// If file:line# is known, use it, else use procedure name up to any parenthesis
const char* GetProcFileName(const char* cmd, char* buffer) {
  FILE* fp  = popen(cmd, "r");

  if (fp == NULL) {
    //fprintf(stderr, "Pipe did not open\n");
    return NULL;
  }

  size_t n = fread(buffer, 1, BUFFSIZE, fp);
  pclose(fp);
  buffer[n] = '\0';

  char* lf = strchr(buffer, LF);
  if (lf != NULL) {lf[0] = '\0';}
  if (lf == NULL) {		// Not even one complete line
    return NULL;
  }
  if (lf == &buffer[n]){	// One line
    return NoArgs(buffer);
  }
  // Two lines, as expected
  char* fileline = &lf[1];
  lf = strchr(fileline, LF);
  if (lf != NULL) {lf[0] = '\0';} 
//VERYTEMP always return routine name
return NoArgs(buffer); 

  if (memcmp(fileline, "??:?", 4) == 0) {
    return NoArgs(buffer);
  }
  return fileline;
}

const char* DoAddr2line(const string& pathname, uint64 offset, char* buffer) {
  char cmd[256];
  sprintf(cmd, "addr2line -fsC -e %s %llx", pathname.c_str(), offset);
  return GetProcFileName(cmd, buffer);
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

void PossiblyReplaceName(OneSpan* onespan, const MapsMap& allmaps) {
  string oldname = onespan->name.substr(4);	// Skip over "PC=
  size_t quote2 = oldname.find("\"");
  if (quote2 != string::npos) {oldname = oldname.substr(0, quote2);}	// Chop trailing "...
  uint64 addr = GetFromHex(oldname);
  if (addr == 0L) {return;}		// Not a hex address that we can map
  const RangeToFile* rtf =  Lookup(onespan->pid, addr, allmaps);
  if (rtf == NULL) {return;}		// Nothing found by lookup

  // We now have the pathname of an executable image containing the address
  // WE ARE NOT DONE YET. This is just the exec file name
  string pathname = rtf->pathname;	
  uint64 offset = addr - rtf->addr_lo;

  // Now execute command: addr2line -fsC -e /lib/x86_64-linux-gnu/libc-2.27.so 0x18eb1f
  // and parse the result into filename:line# or procname 
  char buffer[256];
  const char* newname = DoAddr2line(pathname, offset, buffer);
  if (newname != NULL) {
   // Fixup non-debug libc mapping memcpy into __nss_passwd_lookup
    if (strcmp(newname, "__nss_passwd_lookup") == 0) {newname = "memcpy";}
    onespan->name = string("\"PC=") + newname + "\"],";
    onespan->arg = NameHash(newname);
//fprintf(stdout, "%s => %s\n", oldname.c_str(), newname);
  }
}



// Input is a json file of spans
// start time and duration for each span are in seconds
// Output is a smaller json file of fewer spans with lower-resolution times
void Usage() {
  fprintf(stderr, "Usage: spantopcnameu <pidmaps fname>\n");
  exit(0);
}

//
// Filter from stdin to stdout
//
int main (int argc, const char** argv) {
  if (argc < 2) {Usage();}

  // Input allmaps file
  MapsMap allmaps;

  const char* fname = argv[1];
  FILE* f = fopen(fname, "r");
  if (f == NULL) {
    fprintf(stderr, "%s did not open\n", fname);
    exit(0);
  }
  ReadAllmaps(f, &allmaps);
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

    if (onespan.eventnum == KUTRACE_PC_U) {
      PossiblyReplaceName(&onespan, allmaps);
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
  fprintf(stderr, "spantopcnameu: %d events\n", output_events);

  return 0;
}
