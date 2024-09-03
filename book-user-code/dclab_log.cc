// Simple binary log file format
// This defines a 96-byte binary log record and routines to manipulate it.
// Our dclab client-server routines will use this to log all their activity
//
// Included are routines to create log file names and to print binary log records as ASCII.
//
// Copyright 2021 Richard L. Sites
//

#include <stdio.h>
#include <stdlib.h>     // exit
#include <string.h>
#include <time.h>
#include <unistd.h>     // getpid gethostname
#include <sys/time.h>	// gettimeofday

#include "basetypes.h"
#include "dclab_log.h"
#include "dclab_rpc.h"
//#include "dclab_trace_lib.h"
#include "polynomial.h"


//
// Utility routines
//

// Return floor of log base2 of x, i.e. the number of bits-1 needed to hold x
int32 FloorLg(int32 x) {
  int32 lg = 0;
  int32 local_x = x;
  if (local_x & 0xffff0000) {lg += 16; local_x >>= 16;}
  if (local_x & 0xff00) {lg += 8; local_x >>= 8;}
  if (local_x & 0xf0) {lg += 4; local_x >>= 4;}
  if (local_x & 0xc) {lg += 2; local_x >>= 2;}
  if (local_x & 0x2) {lg += 1; local_x >>= 1;}
  return lg;
}

// Put together an IPv4 address from four separate ints
uint32 MakeIP(int a, int b, int c, int d) {return (a << 24) | (b << 16) | (c << 8) | d;}

// Pad a string out to length using pseudo-random characters.
//  x is a pseudo-random seed and is updated by this routine
//  s is the input character string to be padded and must be allocated big enough
//    to hold at least length characters
//  curlen is the current length of s, bytes to be retained
//  padded_len is the desired new character length
// If curlen >= padded_len, s is returned unchanged. Else it is padded.
// Returns s in both cases.
// DOES NOT return a proper c string with trailing zero byte
char* PadToSimple(uint32* randseed, char* s, int curlen, int padded_len) {
  char* p = s + curlen;	  // First byte off the end;
  for (int i = 0; i < (padded_len - curlen); ++i) {
    if ((i % 5) == 0) {
      *p++ = '_';
    } else {
      *p++ = "abcdefghijklmnopqrstuvwxyz012345"[*randseed & 0x1f];
      *randseed = POLYSHIFT32(*randseed);
    }
  }
  return s;
}


char* PadTo(uint32* randseed, char* s, int baselen, int padded_len) {
  if (baselen >= padded_len) {return s;}

  // Go faster for long strings by just padding out to 256 then copying
  if (padded_len > 256) {
    PadToSimple(randseed, s, baselen, 256);
    for (int i = 256; i <= padded_len - 256; i += 256) {
      memcpy(&s[i], s, 256);
    }
    memcpy(&s[(padded_len >> 8) << 8], s, padded_len & 255);
    return s;
  }

  PadToSimple(randseed, s, baselen, padded_len);
  return s;
}

// String form, updates randseed and str
void PadToStr(uint32* randseed, int padded_len, string* str) {
  int32 baselen = str->size();
  if (baselen >= padded_len) {return;}
  str->resize(padded_len);
  char* str_ptr = const_cast<char*>(str->data());

#if 1
  // Go faster for long strings by just padding out to 256 then copying
  if (padded_len > 256) {
    PadToSimple(randseed, str_ptr, baselen, 256);
    for (int i = 256; i <= padded_len - 256; i += 256) {
      memcpy(&str_ptr[i], str_ptr, 256);
    }
    memcpy(&str_ptr[(padded_len >> 8) << 8], str_ptr, padded_len & 255);
    return;
  }
#endif
 
  PadToSimple(randseed, str_ptr, baselen, padded_len);
}

//
// Formatting for printing
//

// These all use a single static buffer. In real production code, these would 
// all be std::string values, or something else at least as safe.
static const int kMaxDateTimeBuffer = 32;
static char gTempDateTimeBuffer[kMaxDateTimeBuffer];

static const int kMaxPrintBuffer = 256;
static char gTempPrintBuffer[kMaxPrintBuffer];


// Turn seconds since the epoch into yyyymmdd_hhmmss
// Not valid after January 19, 2038
const char* FormatSecondsDateTime(int32 sec) {
  // if (sec == 0) {return "unknown";}  // Longer spelling: caller expecting date
  time_t tt = sec;
  struct tm* t = localtime(&tt);
  sprintf(gTempDateTimeBuffer, "%04d%02d%02d_%02d%02d%02d", 
         t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, 
         t->tm_hour, t->tm_min, t->tm_sec);
  return gTempDateTimeBuffer;
}

// Turn seconds since the epoch into hhmmss (no date)
// Not valid after January 19, 2038
const char* FormatSecondsTime(int32 sec) {
  // if (sec == 0) {return "unk";}  // Shorter spelling: caller expecting no date
  time_t tt = sec;
  struct tm* t = localtime(&tt);
  sprintf(gTempDateTimeBuffer, "%02d%02d%02d", 
         t->tm_hour, t->tm_min, t->tm_sec);
  return gTempDateTimeBuffer;
}

// Turn usec since the epoch into yyyymmdd_hhmmss.usec
const char* FormatUsecDateTime(int64 us) {
  // if (us == 0) {return "unknown";}  // Longer spelling: caller expecting date
  int32 seconds = us / 1000000;
  int32 usec = us - (seconds * 1000000);
  snprintf(gTempPrintBuffer, kMaxPrintBuffer, "%s.%06d", 
           FormatSecondsDateTime(seconds), usec);
  return gTempPrintBuffer;
}

// Turn usec since the epoch into ss.usec (no date)
// Note: initial 3d needed for sort of JSON file to but times in order
const char* FormatUsecTime(int64 us) {
  // if (us == 0) {return "unk";}
  int32 seconds = us / 1000000;
  int32 usec = us - (seconds * 1000000);
  snprintf(gTempPrintBuffer, kMaxPrintBuffer, "%3d.%06d", seconds, usec);
  return gTempPrintBuffer;
}

// TODO: map into a human-meaningful name
const char* FormatIpPort(uint32 ip, uint16 port) {
  if (ip == 0) {return "unk:unk";}
  snprintf(gTempPrintBuffer, kMaxPrintBuffer, "%d.%d.%d.%d:%d", 
           (ip >> 24) & 0xff, (ip >> 16) & 0xff, 
           (ip >> 8) & 0xff, (ip >> 0) & 0xff, port);
  return gTempPrintBuffer;
}

// TODO: map into a human-meaningful name
const char* FormatIp(uint32 ip) {
  if (ip == 0) {return "unk:unk";}
  snprintf(gTempPrintBuffer, kMaxPrintBuffer, "%d.%d.%d.%d", 
           (ip >> 24) & 0xff, (ip >> 16) & 0xff, 
           (ip >> 8) & 0xff, (ip >> 0) & 0xff);
  return gTempPrintBuffer;
}

// Turn RPC type enum into a meaningful name
const char* FormatType(uint32 type) {
  return kRPCTypeName[type];
}

// TenLg length
const char* FormatLglen(uint8 len) {
  snprintf(gTempPrintBuffer, kMaxPrintBuffer, "%d.%d", len / 10, len % 10);
  return gTempPrintBuffer;
}

// Just an rpcid as hex
const char* FormatRPCID(uint32 rpcid) {
  snprintf(gTempPrintBuffer, kMaxPrintBuffer, "%08x", rpcid);
  return gTempPrintBuffer;
}

// Just an rpcid as decimal
const char* FormatRPCIDint(uint32 rpcid) {
  snprintf(gTempPrintBuffer, kMaxPrintBuffer, "%u", rpcid);
  return gTempPrintBuffer;
}

// Method as C string with trailing '\0'
const char* FormatMethod(const char* method) {
  if (method[0] == '\0') {return "unknown";}
  memcpy(gTempPrintBuffer, method, 8);
  gTempPrintBuffer[8] = '\0';
  return gTempPrintBuffer;
}

// Turn status into meaningful name or leave as number
const char* FormatStatus(uint32 status) {
  if (status < NumStatus) {return kRPCStatusName[status];}
  // Unknown status values
  snprintf(gTempPrintBuffer, kMaxPrintBuffer, "ERROR_%d", status);
  return gTempPrintBuffer;
}

// Just show length in decimal
const char* FormatLength(uint32 length) {
  snprintf(gTempPrintBuffer, kMaxPrintBuffer, "%d", length);
  return gTempPrintBuffer;
}

// Turn fixed-field-width data into C string with trailing '\0'
// We expect a delimited string with 4-byte length on front
// We only do the first of possibly two strings
const char* FormatData(const uint8* data, int fixed_width) {
  int trunclen = (fixed_width >= kMaxLogDataSize) ? kMaxLogDataSize : fixed_width;
  for (int i = 0; i < trunclen; ++i) {
    uint8 c = data[i];
    if (c <= ' ') {c = '.';}	// Turn any bytes of delimited length into dots
    gTempPrintBuffer[i] = c;
  }
  gTempPrintBuffer[trunclen] = '\0';

#if 1
  // Suppress trailing spaces
  for (int i = trunclen - 1; i >= 0; --i) {
    if (gTempPrintBuffer[i] == ' ') {
      gTempPrintBuffer[i] = '\0';
    } else {
      break;
    }
  }
#endif
  return gTempPrintBuffer;
}





// In a production environment, use std::string or something safer
static char tempLogFileName[256];

// Construct a name for opening a log file, passing in name of program from command line
//   name is program_time_host_pid
const char* MakeLogFileName(const char* argv0) {
  time_t tt;
  const char* timestr;
  char hostnamestr[256];
  int pid;

  const char* slash = strrchr(argv0, '/');
  // Point to first char of image name
  if (slash == NULL) {
    slash = argv0;
  } else {
    slash = slash + 1;  // over the slash
  }

  tt = time(NULL);
  timestr = FormatSecondsDateTime(tt);
  gethostname(hostnamestr, 256) ;
  hostnamestr[255] = '\0';
  pid = getpid();

  sprintf(tempLogFileName, "%s_%s_%s_%d.log",
          slash, timestr, hostnamestr, pid);
  return tempLogFileName;
}

// Open logfile for writing. Exit program on any error
// Returns the open file. 
FILE* OpenLogFileOrDie(const char* fname) {
  FILE* logfile = fopen(fname, "wb");
  if (logfile == NULL) {
    fprintf(stderr, "%s did not open\n", fname);
    exit(0);
  }
  return logfile;
}




//
// Printing log records
//



// Convenience routine for header of printed log records
void PrintLogRecordHeader(FILE* f) {
  fprintf(f, 
          "REQ_send_time              REQ_rcv_time    RESP_send_time  RESP_rcv_time   "
          "CLIENT_ip:port        SERVER_ip:port        "
          "RPCID    PARENT   "
          "L1  L2  TYPE     "
          "METHOD  STATUS "
          "LEN DATA " 
          "\n");
}


// Print one binary log record to file f
void PrintLogRecord(FILE* f, const BinaryLogRecord* lr) {
  fprintf(f, "%s ", FormatUsecDateTime(lr->req_send_timestamp));
  fprintf(f, "%s ", FormatUsecTime(lr->req_rcv_timestamp));
  fprintf(f, "%s ", FormatUsecTime(lr->resp_send_timestamp));
  fprintf(f, "%s ", FormatUsecTime(lr->resp_rcv_timestamp));

  fprintf(f, "%s ", FormatIpPort(lr->client_ip, lr->client_port));
  fprintf(f, "%s ", FormatIpPort(lr->server_ip, lr->server_port));

  fprintf(f, "%s ", FormatLglen(lr->lglen1));
  fprintf(f, "%s ", FormatLglen(lr->lglen2));
  fprintf(f, "%s ", FormatRPCID(lr->rpcid));
  fprintf(f, "%s ", FormatRPCID(lr->parent));

  fprintf(f, "%s ", FormatType(lr->type));
  fprintf(f, "%s ", FormatMethod(lr->method));
  fprintf(f, "%s ", FormatStatus(lr->status));

  fprintf(f, "%s ", FormatLength(lr->datalength));
  fprintf(f, "%s ", FormatData(lr->data, kMaxLogDataSize));
  fprintf(f, "\n");
}

// Print one binary log record to file f
void PrintLogRecordAsJson(FILE* f, const BinaryLogRecord* lr, uint64 basetime_usec) {
  fprintf(f, "[");
  fprintf(f, "%s, ", FormatUsecTime(lr->req_send_timestamp - basetime_usec));
  fprintf(f, "%s, ", FormatUsecTime(lr->req_rcv_timestamp - basetime_usec));
  fprintf(f, "%s, ", FormatUsecTime(lr->resp_send_timestamp - basetime_usec));
  fprintf(f, "%s, ", FormatUsecTime(lr->resp_rcv_timestamp - basetime_usec));

  //fprintf(f, "\"%s\", ", FormatIpPort(lr->client_ip, lr->client_port));
  //fprintf(f, "\"%s\", ", FormatIpPort(lr->server_ip, lr->server_port));
  fprintf(f, "\"%s\", ", FormatIp(lr->client_ip));
  fprintf(f, "\"%s\", ", FormatIp(lr->server_ip));

  fprintf(f, "%s, ", FormatLglen(lr->lglen1));
  fprintf(f, "%s, ", FormatLglen(lr->lglen2));
  fprintf(f, "%s, ", FormatRPCIDint(lr->rpcid));
  fprintf(f, "%s, ", FormatRPCIDint(lr->parent));

  fprintf(f, "\"%s\", ", FormatType(lr->type));
  fprintf(f, "\"%s\", ", FormatMethod(lr->method));
  fprintf(f, "\"%s\", ", FormatStatus(lr->status));

  fprintf(f, "%s, ", FormatLength(lr->datalength));
  fprintf(f, "\"%s\"", FormatData(lr->data, kMaxLogDataSize));
  fprintf(f, "],\n");
}

void PrintRPC(FILE* f, const RPC* rpc) {
  // Header
  const RPCHeader* hdr = rpc->header;
  fprintf(f, "%s ", FormatUsecDateTime(hdr->req_send_timestamp));
  fprintf(f, "%s ", FormatUsecTime(hdr->req_rcv_timestamp));
  fprintf(f, "%s ", FormatUsecTime(hdr->resp_send_timestamp));
  fprintf(f, "%s ", FormatUsecTime(hdr->resp_rcv_timestamp));

  fprintf(f, "%s ", FormatIpPort(hdr->client_ip, hdr->client_port));
  fprintf(f, "%s ", FormatIpPort(hdr->server_ip, hdr->server_port));
  
  fprintf(f, "%s ", FormatRPCID(hdr->rpcid));
  fprintf(f, "%s ", FormatRPCID(hdr->parent));

  fprintf(f, "%s ", FormatLglen(hdr->lglen1));
  fprintf(f, "%s ", FormatLglen(hdr->lglen2));
  fprintf(f, "%s ", FormatType(hdr->type));

  fprintf(f, "%s ", FormatMethod(hdr->method));
  fprintf(f, "%s ", FormatStatus(hdr->status));
  fprintf(f, "%s ", FormatLength(rpc->datalen));
  fprintf(f, "%s ", FormatData(rpc->data, rpc->datalen));

  fprintf(f, "\n");
}

void RPCToLogRecord(const RPC* rpc, BinaryLogRecord* lr) {
  const RPCHeader* hdr = rpc->header;
  lr->rpcid = hdr->rpcid;
  lr->parent = hdr->parent;
  lr->req_send_timestamp = hdr->req_send_timestamp;
  lr->req_rcv_timestamp = hdr->req_rcv_timestamp;
  lr->resp_send_timestamp = hdr->resp_send_timestamp;
  lr->resp_rcv_timestamp = hdr->resp_rcv_timestamp;

  lr->client_ip = hdr->client_ip;
  lr->client_port = hdr->client_port;
  lr->server_ip = hdr->server_ip;
  lr->server_port = hdr->server_port;

  lr->lglen1 = hdr->lglen1;
  lr->lglen2 = hdr->lglen2;
  lr->type = hdr->type;

  memcpy(lr->method, hdr->method, 8);
  lr->status = hdr->status;
  lr->datalength = rpc->datalen;
  if (rpc->datalen >= kMaxLogDataSize) {
    memcpy(lr->data, rpc->data, kMaxLogDataSize);
  } else {
    memset(lr->data, 0, kMaxLogDataSize);
    memcpy(lr->data, rpc->data, rpc->datalen);
  }
}

void LogRPC(FILE* logfile, const RPC* rpc) {
  BinaryLogRecord lr;
  RPCToLogRecord(rpc, &lr);
  fwrite(&lr, 1, sizeof(BinaryLogRecord), logfile);
}


// Print error message to stderr from system errno and terminate
void Error(const char* msg) {
  perror(msg);
  exit(EXIT_FAILURE);
}

// Print error message to stderr from supplied errornum and terminate
void Error(const char* msg, int errornum) {
  fprintf(stderr, "%s: %s\n", msg, strerror(errornum));
  exit(EXIT_FAILURE);
}

// Print error message to stderr from supplied msg2 and terminate
void Error(const char* msg, const char* msg2) {
  fprintf(stderr, "%s: %s\n", msg, msg2);
  exit(EXIT_FAILURE);
}

// Print error message to stderr from system errno and return
void ErrorNoFail(const char* msg) {
  perror(msg);
}


