// client4.cc cloned from client2.cc 2018.04.16
// Copyright 2021 Richard L. Sites
//
// compile with g++ -O2 -pthread client4.cc dclab_log.cc dclab_rpc.cc kutrace_lib.cc -o client4
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h> 
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "basetypes.h"
#include "dclab_log.h"
#include "dclab_rpc.h"
#include "kutrace_lib.h"
#include "polynomial.h"
#include "spinlock.h"
#include "timecounters.h"

// Cheap globals for local statistics and logging

// Response time in usec histogram buckets of floor lg usec
static uint32 hist[32];
static int64 rpc_count;
static int64 total_usec;
static int64 txbytes;
static int64 rxbytes;

static bool verbose;
static FILE* logfile;
static uint32 server_ipnum;
static uint16 server_portnum;

// Global for fast sink. Build value once and then reuse
static string sink_value;
 
inline uint32 NextRand(uint32* seed) {
  *seed = POLYSHIFT32(*seed);
  return *seed;
}

void WaitMsec(int32 msec) {
  struct timespec tv;
  tv.tv_sec = msec / 1000;
  tv.tv_nsec = (msec % 1000) * 1000000;
  nanosleep(&tv, NULL);
}

 
int ConnectToServer(const char* server_name, const char* server_port, 
                    uint32* server_ipnum, uint16* server_portnum) {
  struct addrinfo hints;
  struct addrinfo* server;
  int sockfd;
  int iret;

  // First, load up address structs with getaddrinfo():
  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_INET;		// IPv4
  hints.ai_socktype = SOCK_STREAM;	// TCP
  iret = getaddrinfo(server_name, server_port, &hints, &server);
  if (iret != 0) {Error("getaddrinfo",  gai_strerror(iret));}

  // Make a socket:
  sockfd = socket(server->ai_family, server->ai_socktype, server->ai_protocol);
  if (sockfd < 0) {Error("socket");}

  // Connect
  iret = connect(sockfd, server->ai_addr, server->ai_addrlen);
  if (iret < 0) {Error("connect");}
  const sockaddr_in* sin = reinterpret_cast<const sockaddr_in*>(server->ai_addr);
  fprintf(stderr, "at client, server IP = %08x:%04x\n", ntohl(sin->sin_addr.s_addr), ntohs(sin->sin_port));
  *server_ipnum = ntohl(sin->sin_addr.s_addr);
  *server_portnum = ntohs(sin->sin_port);

  return sockfd;
}

// Send one RPC over the wire: marker, header, data
bool SendRequest(int sockfd, RPC* req) {
  return SendOneRPC(sockfd, req, NULL);
}

bool ReceiveResponse(int sockfd, RPC* response) {
  ReadOneRPC(sockfd, response, NULL);
  return true;
}

void PrintResponse(FILE* f, RPC* response) {
  // If we have the response to a stats request, print it here onto stdout
  if ((strcmp(response->header->method, "stats") == 0) && (response->datalen >= 4)) {
    const uint8* temp = response->data;
    string stats = GetStringArg(&temp);
    fprintf(stdout, "%s\n", stats.c_str());
  }
}


void IncrString(char* s) {
  int len = strlen(s);
  for (int i = len - 1; i >= 0; ++i) {
    char c = s[i];
    if (c == '9') {s[i] = '0';} 
    else if (c == 'z') {s[i] = 'a';} 
    else if (c == 'Z') {s[i] = 'A';} 
    else if (c > 0x7e) {s[i] = 0x21;} 
    else {s[i] += 1; return;}
  }
}

void IncrString(string* s) {
  for (int i = s->size() - 1; i >= 0; --i) {
    char c = (*s)[i];
    if (c == '9') {(*s)[i] = '0';} 
    else if (c == 'z') {(*s)[i] = 'a';} 
    else if (c == 'Z') {(*s)[i] = 'A';} 
    else if (c > 0x7e) {(*s)[i] = 0x21;} 
    else {(*s)[i] += 1; return;}
  }
  // We can fall out if we roll over, such as 9999 to 0000. All is OK.
}


// The working-on-RPC events KUTRACE_RPCIDREQ and KUTRACE_RPCIDRESP have this 
// format:
// +-------------------+-----------+---------------+-------+-------+
// | timestamp 2       | event     |     lglen8    |     RPCid     | (2)
// +-------------------+-----------+---------------+-------+-------+
//          20              12         8       8           16 

bool SendCommand(int sockfd, uint32* randseed, const char* command, 
                 string& key_base_str, int key_padlen, 
                 string& value_base_str, int value_padlen) {
  bool ok = true;
  string value;
  string* val;
  // Expand string values as needed
  string key = key_base_str;
  PadToStr(randseed, key_padlen, &key);
  // Cache sink_value after first time, for speed
  bool sink_command = (strcmp(command, "sink") == 0);
  if ((sink_value.empty()) || !sink_command) {
    val = &value;
    value = value_base_str;
    PadToStr(randseed, value_padlen, &value);
    if (sink_command) {sink_value = value;}
  } else {
    val = &sink_value;
  }

  // Build request header, setting RPCID
  RPCHeader rpcheader;
  memset(&rpcheader, 0, sizeof(RPCHeader));
  rpcheader.type = ReqSendType;
  rpcheader.server_ip = server_ipnum;
  rpcheader.server_port = server_portnum;
  rpcheader.rpcid = NextRand(randseed);
  rpcheader.parent = 0;
  strncpy(&rpcheader.method[0], command, 8);
 
  RPC request;
  memset(&request, 0, sizeof(RPC));
  request.header = &rpcheader;
  request.headerlen = sizeof(RPCHeader);

  if ((key.size() > 0) && (val->size() > 0)) {
    PutStringRPC2(key, *val, &request);
  } else if (key.size() > 0) {
    PutStringRPC(key, &request);
  } else if (val->size() > 0) {
    PutStringRPC(*val, &request);
  }

  uint8 lglen8 = TenLg(request.datalen);
  request.header->lglen1 = lglen8;	// Request length
  request.header->type = ReqSendType;
  request.header->req_send_timestamp = GetUsec();		// T1

  // Trace the outgoing RPC request
  // RPCid is pseudo-random 32 bits, but never zero. If low 16 bits are zero, send high 16 bits.
  uint32 tempid = rpcid32_to_rpcid16(request.header->rpcid);

  // rls 2020.08.23 record the method name for each outgoing RPC
  // We also pack in the request length
  kutrace::addname(KUTRACE_METHODNAME, tempid, request.header->method);

  // Start tracing the outgoing RPC request
  kutrace::addevent(KUTRACE_RPCIDREQ, (lglen8 << 16) | tempid);

  if (verbose) {fprintf(stdout, "client4: SendRequest:     "); PrintRPC(stdout, &request);}
  LogRPC(logfile, &request);


  ok &= SendRequest(sockfd, &request);

  // Stop tracing the outgoing RPC request
  kutrace::addevent(KUTRACE_RPCIDREQ, 0);

  // Block here until the response comes back
  RPC response;
  ok &= ReceiveResponse(sockfd, &response);

  int64 resp_rcv_time = GetUsec();				// T4
  response.header->resp_rcv_timestamp = resp_rcv_time;
  response.header->type = RespRcvType;

  // Trace the incoming RPC response
  // RPCid is pseudo-random 32 bits, but never zero. If low 16 bits are zero, use high 16 bits.
  tempid = rpcid32_to_rpcid16(response.header->rpcid);
  lglen8 = response.header->lglen2;	// Respense length

  // Start tracing the incoming RPC response
  kutrace::addevent(KUTRACE_RPCIDRESP, (lglen8 << 16) | tempid);   

  if (verbose) {fprintf(stdout, "client4: ReceiveResponse: "); PrintRPC(stdout, &response);}
  LogRPC(logfile, &response);
  int64 elapsed = resp_rcv_time - response.header->req_send_timestamp;
 
  // Print first 20 round-trip times in msec
  if (rpc_count < 20) {
    fprintf(stdout, "%5.3fms  ", elapsed / 1000.0);
    if ((rpc_count % 10) == 9) {fprintf(stdout, "\n");}
  }
  // gather some simple statistics
  int32 usec = elapsed;
  ++hist[FloorLg(usec)];
  ++rpc_count;
  total_usec += elapsed;
  txbytes += sizeof(RPCMarker) + request.headerlen + request.datalen;
  rxbytes += sizeof(RPCMarker) + response.headerlen + response.datalen;

  PrintResponse(stdout, &response);
  FreeRPCDataOnly(&request);
  FreeRPC(&response);

  // Stop tracing the incoming RPC response
  kutrace::addevent(KUTRACE_RPCIDRESP, 0);

  return ok;
}

bool SendQuit(int sockfd, uint32* randseed) {
  bool ok = true;
  RPCHeader rpcheader;
  memset(&rpcheader, 0, sizeof(RPCHeader));
  rpcheader.type = ReqSendType;
  rpcheader.server_ip = server_ipnum;
  rpcheader.server_port = server_portnum;
  rpcheader.rpcid = NextRand(randseed);
  rpcheader.parent = 0;
  strncpy(&rpcheader.method[0], "quit", 8);
 
  RPC request;
  memset(&request, 0, sizeof(RPC));
  request.header = &rpcheader;
  request.headerlen = sizeof(RPCHeader);

  request.header->req_send_timestamp = GetUsec();
  request.header->type = ReqSendType;
  LogRPC(logfile, &request);
  if (verbose) {fprintf(stdout, "client4: SendRequest:     "); PrintRPC(stdout, &request);}
  ok &= SendRequest(sockfd, &request);

  // Block here until the response comes back
  RPC response;
  ok &= ReceiveResponse(sockfd, &response);
  int64 resp_rcv_time = GetUsec();
  response.header->resp_rcv_timestamp = resp_rcv_time;
  response.header->type = RespRcvType;
  LogRPC(logfile, &response);
  if (verbose) {fprintf(stdout, "client4: ReceiveResponse: "); PrintRPC(stdout, &response);}
  int64 elapsed = resp_rcv_time - response.header->req_send_timestamp;

  PrintResponse(stdout, &response);
  FreeRPCDataOnly(&request);
  FreeRPC(&response);
  return ok;
} 


void Usage() {
  fprintf(stderr, 
    "Usage: client4 server port [-rep number] [-k number] [-waitms number] [-verbose] [-seed1]\n"
    "               command [-key \"keybase\" [+] [padlen]]  [-value \"valuebase\" [+] [padlen]]\n");
  fprintf(stderr, "       command: ping [-value \"valuebase\" [+] [padlen]]\n");
  fprintf(stderr, "       command: read  -key \"keybase\" [+] [padlen]\n");
  fprintf(stderr, "       command: write  -key \"keybase\" [+] [padlen]  -value \"valuebase\" [+] [padlen]\n");
  fprintf(stderr, "       command: sink   -key \"keybase\" [+] [padlen]  -value \"valuebase\" [+] [padlen]\n");
  fprintf(stderr, "       command: delete  -key \"keybase\" [+] [padlen]\n");
  fprintf(stderr, "       command: stats \n");
  fprintf(stderr, "       command: reset \n");
  fprintf(stderr, "       command: quit \n");
  exit(EXIT_FAILURE);
}

// Our little client language, examples
//
// Our key or value field consists of 
//   a base string
//   an optional + parameter indicating that the base is to be incremented each time
//   an optional pad length parameter indicating that pseudo-random characters are to 
//     be appended each time (Note that padding the key field is suspect, because those
//     keys cannot be reproduced for subsequent commands.)
//
// client2 dclab-1.epfl.ch 12345 ping
// client2 dclab-1.epfl.ch 12345 ping -value "vvvv" + 4000
// client2 dclab-1.epfl.ch 12345 -rep 10 -k 5  read -key "kkkkk" + -waitms 4
// client2 dclab-1.epfl.ch 12345 -rep 10 -k 5 write -key "kkkkk" + -value "vvvv" + 20 -waitms 100
// client2 dclab-1.epfl.ch 12345 delete -k 5 delete -key "kkkkk" +
// client2 dclab-1.epfl.ch 12345 stats
// client2 dclab-1.epfl.ch 12345 reset
// client2 dclab-1.epfl.ch 12345 quit
 

int main (int argc, const char** argv) {
  // Command-line argument variables and their defaults
  int32 outer_repeats = 1;
  int32 inner_repeats = 1;
  int32 wait_msec = 0;
  const char* command = NULL;	// command is required
  const char* key_base = "";
  const char* value_base = "";
  int key_padlen = 0;
  int value_padlen = 0;
  bool key_incr = false;
  bool value_incr = false;
  verbose = false;
  uint32 randseed = 1;
  bool seed1 = false;		// If true, set seed to 1 every time for repeatability
  sink_value.clear();

  // Get the command-line arguments
  // Server name as text is argv[1] and server port as text is argv[2]. Start here looking at [3]
  if (argc < 4) { Usage(); } 
  
  for (int i = 3; i < argc; ++i) {
    if(strcmp(argv[i], "-rep") == 0) {if (i + 1 < argc) {outer_repeats = atoi(argv[i + 1]); ++i;}}
    else if(strcmp(argv[i], "-k") == 0) {if (i + 1 < argc) {inner_repeats = atoi(argv[i + 1]); ++i;}}
    else if(strcmp(argv[i], "-key") == 0) {
      if (i + 1 < argc) {key_base = argv[i + 1]; ++i;}
      if ((i + 1 < argc) && (argv[i + 1][0] == '+')) {key_incr = true; ++i;}
      if ((i + 1 < argc) && (argv[i + 1][0] != '-')) {key_padlen = atoi(argv[i + 1]); ++i;}
    }
    else if(strcmp(argv[i], "-value") == 0) {
      if (i + 1 < argc) {value_base = argv[i + 1]; ++i;}
      if ((i + 1 < argc) && (argv[i + 1][0] == '+')) {value_incr = true; ++i;}
      if ((i + 1 < argc) && (argv[i + 1][0] != '-')) {value_padlen = atoi(argv[i + 1]); ++i;}
    }
    else if(strcmp(argv[i], "-waitms") == 0) {if (i + 1 < argc) {wait_msec = atoi(argv[i + 1]); ++i;}}
    else if (strcmp(argv[i], "-verbose") == 0) {verbose = true;}
    else if (strcmp(argv[i], "-seed1") == 0) {seed1 = true;}
    // Bare word is command if we haven't seen one yet
    else if ((argv[i][0] != '-') && (command == NULL)) {command = argv[i];}
    else {fprintf(stderr, "Bad token at argv[%d] %s\n", i, argv[i]); Usage();}
  }

  if (command == NULL) {fprintf(stderr, "No command\n"); Usage();}
  if ((strcmp(command, "read") == 0) && (key_base[0] =='\0')) {fprintf(stderr, "Missing -key for read\n"); Usage();}
  if ((strcmp(command, "write") == 0) && (key_base[0] =='\0')) {fprintf(stderr, "Missing -key for write\n"); Usage();}
  if ((strcmp(command, "sink") == 0) && (key_base[0] =='\0')) {fprintf(stderr, "Missing -key for sink\n"); Usage();}
  if ((strcmp(command, "delete") == 0) && (key_base[0] =='\0')) {fprintf(stderr, "Missing -key for delete\n"); Usage();}
  if ((strcmp(command, "write") == 0) && (value_base[0] =='\0')) {fprintf(stderr, "Missing -value for write\n"); Usage();}
  if ((strcmp(command, "sink") == 0) && (value_base[0] =='\0')) {fprintf(stderr, "Missing -value for sink\n"); Usage();}

  // fprintf(stdout, "outer %d, inner %d, wait %d, cmd %s, key %s, val %s, kpad %d, vpad %d, kincr %d, vincr %d, verbose %d\n", 
  //      outer_repeats, inner_repeats, wait_msec, command, key_base, value_base, key_padlen, value_padlen, key_incr, value_incr, verbose);

  // Initialize globals for local statistics
  memset(hist, 0, sizeof(hist));
  rpc_count = 0;
  total_usec = 0;
  txbytes = 0;
  rxbytes = 0;

  const char* logfilename = MakeLogFileName(argv[0]);
  logfile = OpenLogFileOrDie(logfilename);

  // Initialize pseudo-random generator based on process id and time
  uint32 pid = getpid();                      
  randseed = time(NULL) ^ (pid << 16) ;	// Different seed each run
  if (randseed == 0) {randseed = POLYINIT32;}	// Safety move to avoid accidental seed=0
  if (seed1) {randseed = 1;}

  // Copy key_base and value_base so they can be incremented
  string key_base_str = string(key_base);
  string value_base_str = string(value_base);
  bool ok = true;
  bool sink_command = (strcmp(command, "sink") == 0);

  int sockfd = ConnectToServer(argv[1], argv[2], &server_ipnum, &server_portnum);

  // The double-nested command loop
  for (int i = 0; i < outer_repeats; ++i) {
    if (sink_command) {kutrace::mark_d(value_padlen + i);}
    for (int j = 0; j < inner_repeats; ++j) {
      SendCommand(sockfd, &randseed, command, key_base_str, key_padlen, value_base_str, value_padlen);
      if (key_incr) {IncrString(&key_base_str);}
      if (value_incr) {IncrString(&value_base_str);}
    }
    WaitMsec(wait_msec);
  }

  close(sockfd);


  // Print some summary statistics
  fprintf(stderr, "\n");
  fprintf(stderr, "Histogram of floor log 2 buckets of usec response times\n");
  fprintf(stderr, "1 2+ 4+ us            1+ 2+ 4+ msec         1+ 2+ 4+ sec           1K+ 2k+ secs\n");
  fprintf(stderr, "|                     |                     |                      |\n");
  for (int i = 0; i < 32; ++i) {
    fprintf(stderr, "%d ", hist[i]);
    if ((i % 10) == 9) {fprintf(stderr, "  ");}
  }
  fprintf(stderr, "\n");
  fprintf(stderr, "%lld RPCs, %5.1f msec, %5.3f TxMB, %5.3f RxMB total\n", 
          rpc_count, total_usec / 1000.0, txbytes / 1000000.0, rxbytes / 1000000.0);
  fprintf(stderr, 
          "%5.1f RPC/s (%5.3f msec/RPC), %5.1f TxMB/s, %5.1f RxMB/s\n", 
          (rpc_count * 1000000.0) / total_usec, 
          (total_usec * 0.001) / rpc_count,
          (txbytes * 1.0) / total_usec, 
          (rxbytes * 1.0) / total_usec);
  fprintf(stderr, "\n");

  fclose(logfile);
  fprintf(stderr, "%s written\n", logfilename);


  return EXIT_SUCCESS;
}


