// Simple binary log file format
// This defines a 96-byte binary log record and routines to manipulate it.
// Our dclab client-server routines will use this to log all their activity
//
// Included are routines to create log file names and to print binary log records as ASCII.
//
// Copyright 2021 Richard L. Sites

#ifndef __DCLAB_LOG_H__
#define __DCLAB_LOG_H__

#include <stdio.h>

# include <string>

#include "basetypes.h"
#include "dclab_rpc.h"

using std::string;

static const int kMaxLogDataSize = 24;

typedef struct {
  uint32 rpcid;
  uint32 parent;
  int64 req_send_timestamp;	// usec since the epoch, client clock
  int64 req_rcv_timestamp;	// usec since the epoch, server clock
  int64 resp_send_timestamp;	// usec since the epoch, server clock
  int64 resp_rcv_timestamp;	// usec since the epoch, client clock
  // 40 bytes

  uint32 client_ip;
  uint32 server_ip;
  uint16 client_port;
  uint16 server_port;
  uint8 lglen1;			// 10 * lg(request data length in bytes)
  uint8 lglen2;			// 10 * lg(response data length in bytes)
  uint16 type;			// An RPCType 
  // 16 bytes

  char method[8];
  // 64 bytes

  uint32 status;		// 0 = success, other = error code
  uint32 datalength;		// full length transmitted
  // 72 bytes to here

  uint8 data[kMaxLogDataSize];	// truncated, zero filled 
  // 96 bytes
} BinaryLogRecord;


// Utility routines

// Return floor of log base2 of x, i.e. the number of bits needed to hold x
int32 FloorLg(int32 x);

// Put together an IPv4 address from four separate ints
uint32 MakeIP(int a, int b, int c, int d);

// Turn IPv4:port into a printable string
const char* FormatIpPort(uint32 ip, uint16 port);


// Pad a string out to length using pseudo-random characters.
//  x is a pseduo-random seed and is updated by this routine
//  s is the input character string to be padded and must be allocated big enough
//    to hold at least length characters
//  curlen is the current length of s, bytes to be retained
//  padded_len is the desired new character length
// If curlen >= padded_len, s is returned unchanged. Else it is padded.
// Returns s in both cases.
char* PadTo(uint32* x, char* s, int curlen, int padded_len);

// String form, updates randseed and str
void PadToStr(uint32* randseed, int padded_len, string* str);


// Construct a name for opening a log file, passing in name of program from command line
// Returns the resulting name, which is of the form program_time_host_pid
const char* MakeLogFileName(const char* argv0);

// Open logfile for writing. Exit program on any error
// Returns the open file. 
FILE* OpenLogFileOrDie(const char* fname);


// Convenience routine for header of printed log records
void PrintLogRecordHeader(FILE* f);

// Print one binary log record to file f
void PrintLogRecord(FILE* f, const BinaryLogRecord* lr);
void PrintLogRecordAsJson(FILE* f, const BinaryLogRecord* lr, uint64 basetime_usec);
void PrintRPC(FILE* f, const RPC* rpc);
void RPCToLogRecord(const RPC* rpc, BinaryLogRecord* lr);
void LogRPC(FILE* logfile, const RPC* rpc);

// Print error messages to stderr
void Error(const char* msg);
void Error(const char* msg, int errornum);
void Error(const char* msg, const char* msg2);
void ErrorNoFail(const char* msg);

#endif	// __DCLAB_LOG_H__



