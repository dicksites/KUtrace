// server_mystery21.cc cloned from server4.cc 2018.07.08
//  Serve dummy database from disk not RAM
//  Uses /tmp/keyvaluestore which will be created if not there
// Copyright 2021 Richard L. Sites
//
// dick sites 2019.09.27 Add fake one-entry "cache" for timing. No way to invalidate it.
// dick sites 2020.02.09 Change to WeirdChecksum to deliberately vary execution time
//
// compile with 
// g++ -O2 -pthread server_mystery21.cc dclab_log.cc dclab_rpc.cc kutrace_lib.cc spinlock_fixed.cc -o server_mystery21 

#include <errno.h>
#include <ftw.h>		// For nftw file tree walk
#include <pthread.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>		// for nanosleep
#include <unistd.h>		// read(), stat, mkdir
#include <netinet/in.h> 
#include <netinet/ip.h> 	/* superset of previous */
#include <netinet/tcp.h>
#include <sys/socket.h> 
#include <sys/stat.h>		// For stat/mkdir
#include <sys/types.h>		// For stat/mkdir, others

// For -direct disk read/write
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <map>
#include <string>

#include "basetypes.h"
#include "dclab_log.h"
#include "dclab_rpc.h"
#include "kutrace_lib.h"
#include "polynomial.h"
#include "spinlock.h"
#include "timecounters.h"

using std::map;
using std::string;


// Should generate a single rotate instruction when available
inline uint32_t rotl32 (uint32_t x, uint32_t n)
{
  return (x << n) | (x >> (32-n));
}

typedef map<string, string> MapKeyValue;

typedef struct {
  LockAndHist lockandhist;
  string directory;		// String for easy concatenate
  const char* logfilename;
  FILE* logfile;
  MapKeyValue key_value;
} SharedData;

typedef struct {
  string cached_key;
  string cached_value;
  uint16 portnum;
  SharedData* shareddata;
} PerThreadData;

static const int kMaxRunTimeSeconds = 4 * 60;

static const char* kDirectoryName = "/tmp/keyvaluestore";

static const int kPageSize = 4096;		// Must be a power of two
static const int kPageSizeMask = kPageSize - 1;

// Must be a multiple of 4KB
static const int kMaxValueSize = 1025 * 1024;	// 1MB + 1KB extra


// Global flags
static bool direct = false;	// If true, read/write O_DIRECT O_SYNC
static bool verbose = false;
static bool verbose_data = false;
static bool stopping = false;	// Any thread can set this true
static int wait_msec = 0;	// Extra time to hold lock, for extra interference

// Wait n msec
void WaitMsec(int msec) {
  if (msec == 0) {return;}
  struct timespec req;
  req.tv_sec = msec / 1000;
  req.tv_nsec = (msec % 1000) * 1000000;
  nanosleep(&req, NULL);
}


int OpenSocket(int16 portnum) {
  // Open a TCP/IPv4 socket. 
  // Returns file descriptor if OK, -1 and sets errno if bad
  //fprintf(stderr, "server_mystery21: Open server socket\n");
  int sockfd = socket(AF_INET, SOCK_STREAM, 0);  
  if (sockfd < 0) {Error("Socket open");}

  // Bind this socket to a particular TCP/IP port.
  // Construct server address structure first
  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;		// TCP/IPv4
  server_addr.sin_addr.s_addr = INADDR_ANY;	// Find out my IP address
  server_addr.sin_port = htons(portnum);	// host-to-network of short int portnum
 
  //fprintf(stderr, "server_mystery21: Bind server socket, port %d\n", portnum);
  int iretbind = bind(sockfd, 
                      reinterpret_cast<struct sockaddr*>(&server_addr), 
                      sizeof(server_addr));
  if (iretbind != 0) {Error("Bind socket");}
  //fprintf(stderr, "server_mystery21: Bound server socket %08x:%04x\n", server_addr.sin_addr.s_addr, server_addr.sin_port);

  return sockfd;
}

// Accept a client connection and return the new socket fd
int ConnectToClient(int sockfd, uint32* client_ip, uint16* client_port) {
  // Listen on the bound port for a connection attempt.
  // Allow the default maximum 5 simultaneous attempts (four of which would wait)A
  //fprintf(stderr, "server_mystery21: listen server socket\n");
  int iretlisten = listen(sockfd, 5);
  if (iretlisten != 0) {Error("listen");}

  // Accept an incoming connection
  // Reserve client address structure first
  // This blocks indefinitely, until a conneciton is tried from some client
  struct sockaddr_in client_addr;
  memset(&client_addr, 0, sizeof(client_addr));

  socklen_t clientlen = sizeof(client_addr);	// This will get changed to actual client len by accept                       
  int acceptsockfd = accept(sockfd, reinterpret_cast<struct sockaddr*>(&client_addr), &clientlen);
  if (acceptsockfd < 0) {Error("accept");}
  const sockaddr_in* sin = reinterpret_cast<const sockaddr_in*>(&client_addr);
  *client_ip = ntohl(sin->sin_addr.s_addr);
  *client_port = ntohs(sin->sin_port);

  // We have a connection from some client machine
  fprintf(stderr, "server_mystery21: connection from %s\n", FormatIpPort(*client_ip, *client_port));
  return acceptsockfd;
}

bool ReceiveRequest(int sockfd, RPC* req) {
  return ReadOneRPC(sockfd, req, NULL);
}

bool SendResponse(int sockfd, RPC* resp) {
  SendOneRPC(sockfd, resp, NULL);
  return true;
}

string StringPrintf(const char* format, ...) {
  va_list args;
  char buffer[256];
  va_start(args, format);
  int n = vsnprintf(buffer, 256, format, args);
  va_end(args);
  if (n < 0) {return string("");}
  return string(buffer, n);
}

// Create the repsonse to a ping request
// Returns false on any error, with response status indicating why
bool DoPing(SharedData* shareddata, const RPC* request, RPC* response) {
  // We just send the data back unchanged
  CopyRPCData(request, response);		
  return true;
}


// Do a simple checksum of a string, returning a short string
char* Checksum(const char* s, char* chksumbuf) {
  uint32 sum = 0;
  int len = strlen(s);
  for (int i = 0; i < (len >> 2); i += 4) {
    sum = rotl32(sum, 3) + *reinterpret_cast<const uint32*>(&s[i]);
  }
  // Tail end if any
  if (len & 3) {
    char temp[4];
    memset(temp, 0, 4);
    memcpy(temp, &s[len & ~3], len & 3);
    sum = rotl32(sum, 3) + *reinterpret_cast<const uint32*>(temp);
  }
  // Now make a short character string output
  memset(chksumbuf, 0, 16);
  sprintf(chksumbuf, "%08x", sum);
  // fprintf(stderr, "Checksum(%d) = '%s'\n", len, chksumbuf);
  return chksumbuf;
}

static const uint8* cryptkey = (const uint8*)"prettygoodkeyphrase";

// Do a simple RC4 decryption of a string, returning a short string
char* DecryptingChecksum(const char* s, char* chksumbuf) {
  int keylength = strlen((const char*)cryptkey);
  int i, j;
  // Set up key schedule
  uint8 perm[256];
  for (i = 0; i < 256; ++i) {perm[i] = i;}
  j = 0;
  for (i = 0; i < 256; ++i) {
    j = (j + perm[i] + cryptkey[i % keylength]) & 255;
    uint8 temp = perm[i]; perm[i] = perm[j]; perm[j] = temp;
  }

  // Generate output and xor with s
  i = 0;
  j = 0;
  uint32 sum = 0;
  int len = strlen(s);
  for (int n = 0; n < len; ++n) {
    i = (i + 1) & 255;
    j = (j + perm[i]) & 255; 
    uint8 temp = perm[i]; perm[i] = perm[j]; perm[j] = temp;
    uint8 k = perm[(perm[i] + perm[j]) & 255];
    sum += (s[i] ^ k);
  }

  // Now make a short character string output
  memset(chksumbuf, 0, 16);
  sprintf(chksumbuf, "%08x", sum);
  // fprintf(stderr, "Checksum(%d) = '%s'\n", len, chksumbuf);
  return chksumbuf;
}

// Pseudo-random value
//uint32 polyx = POLYINIT32;
uint32 polyx = 1234567890;

// True always
bool SomeComplexBusinessLogic(uint32 polyx) {
  return true;
}

// True 1 time out of 64
bool OtherBusinessLogic(uint32 polyx) {
  return (polyx & 63) == 0;
}

// True 1 time out of 5
bool WrongBusinessLogic(uint32 polyx) {
  return (polyx % 5) == 0;
}

// A checksum routine that deliberately varies how long it takes from call to call
char* WeirdChecksum(const char* s, char* chksumbuf) {
  char* retval = NULL;
  if (SomeComplexBusinessLogic(polyx)) {
    // The case we are testing
    if (OtherBusinessLogic(polyx)) {
      // 1 of 64, slow processing
   kutrace::mark_b("decryp");
      for (int n = 0; n < 10; ++n) {retval = DecryptingChecksum(s, chksumbuf);}
    } else {
      // 63 of 64, normal processing
   kutrace::mark_b("chk");
      for (int n = 0; n < 10; ++n) {retval = Checksum(s, chksumbuf);}
    }
    if (WrongBusinessLogic(polyx)) {
      // 1 of 5, medium processing
   kutrace::mark_b("chk");
      for (int n = 0; n < 10; ++n) {retval = Checksum(s, chksumbuf);}
   kutrace::mark_b("chk");
      for (int n = 0; n < 10; ++n) {retval = Checksum(s, chksumbuf);}
    }
  } else {
    // .. other cases that never happen
    retval = Checksum(s, chksumbuf);
  } 

  // Update our pseudo-random business logic
  polyx = POLYSHIFT32(polyx);
  polyx = POLYSHIFT32(polyx);
  return retval;
}

// Allocate a byte array of given size, aligned on a page boundary
// Caller will call free(rawptr)
uint8* AllocPageAligned(int bytesize, uint8** rawptr) {
  int newsize = bytesize + kPageSizeMask;
  *rawptr = reinterpret_cast<uint8*>(malloc(newsize));
  uintptr_t temp = reinterpret_cast<uintptr_t>(*rawptr);
  uintptr_t temp2 = (temp + kPageSizeMask) & ~kPageSizeMask;
  return  reinterpret_cast<uint8*>(temp2);
}



bool BufferedRead(string fname, uint8* buffer, int maxsize, int* n) {
  *n = 0;
  FILE* f = fopen(fname.c_str(), "rb");
  if (f == NULL) {
    return false;
  }
  // Read the value
  *n = fread(buffer, 1, maxsize, f);
  fclose(f);
  // If n is max buffersize, then assume value is too long so fail
  if (*n == maxsize) {
    return false;
  }
  return true;
}

// Note: O_DIRECT must transfer multiples of 4KB into aligned buffer
bool DirectRead(string fname, uint8* buffer, int maxsize, int* n) {
  *n = 0;
  int fd = open(fname.c_str(), O_RDONLY | O_NOATIME | O_DIRECT | O_SYNC);
  if (fd < 0) {
    perror("DirectRead open");
    return false;
  }
  // Read the value
  *n = read(fd, buffer, maxsize);
  close(fd);
  // If n is max buffersize, then assume value is too long so fail
  if ((*n < 0) || (*n == maxsize)) {
    perror("DirectRead read");
    return false;
  }
  return true;
}


bool BufferedWrite(string fname, const uint8* buffer, int size, int* n) {
  *n = 0;
  FILE* f = fopen(fname.c_str(), "wb");
  if (f == NULL) {
    return false;
  }
  // Write the value
  *n = fwrite(buffer, 1, size, f);
  fclose(f);
  if (*n != size) {
    return false;
  }
  return true;
}

//  int fd = open(filename, O_WRONLY | O_CREAT | O_DIRECT | O_SYNC, S_IRWXU);
//  if (fd < 0) {perror("server_mystery21 write open"); return;}



// Note: O_DIRECT must transfer multiples of 4KB into aligned buffer
bool DirectWrite(string fname, const uint8* buffer, int size, int* n) {
  *n = 0;
  int fd = open(fname.c_str(), O_WRONLY | O_CREAT | O_DIRECT | O_SYNC, S_IRWXU);
  if (fd < 0) {
    perror("DirectWrite open");
    return false;
  }
  // Read the value
  *n = write(fd, buffer, size);
  close(fd);
  // If n is max buffersize, then assume value is too long so fail
  if (*n != size) {
    perror("DirectWrite write");
    return false;
  }
  return true;
}



// Read <key> from request argument
// Set result data to <value>
// Return status=fail and empty data if key is not found
bool DoRead(PerThreadData* perthreaddata,
            SharedData* shareddata, const RPC* request, RPC* response) {
  const uint8* req_data = request->data; 
  string key = GetStringArg(&req_data);
  string value;

  // Dummy one-entry "cache". There is no mechanism to invaladate old data after a write
  if (perthreaddata->cached_key == key) {
    // Hit
    ////fprintf(stdout, "%s hit\n", key.c_str());
    PutStringRPC(perthreaddata->cached_value, response);
    return true;
  }

  {
    SpinLock sp(&shareddata->lockandhist);
    // Open key on disk, if any. read/close
    string fname = shareddata->directory + "/" + key;
    // fprintf(stdout, "  \"%s\"\n", fname.c_str());

    uint8* rawptr;
    uint8* ptr = AllocPageAligned(kMaxValueSize, &rawptr);
    bool ok;
    int n;
   kutrace::mark_b("disk");
    if (direct) {
      ok = DirectRead(fname, ptr, kMaxValueSize, &n);
    } else {
      ok = BufferedRead(fname, ptr, kMaxValueSize, &n);
    }
   kutrace::mark_b("/disk");
    if (!ok) {
      response->header->status = FailStatus;	// Let the caller know key wasn't there
      free(rawptr);
      return true;
    }
    // Cache the <key, value> pair
    perthreaddata->cached_key = key;
    perthreaddata->cached_value = string((char*)ptr, n);

    PutStringRPC(perthreaddata->cached_value, response);
    free(rawptr);

    WaitMsec(wait_msec);
  }

  return true;
}

// Read but then return just a simple checksum of the value
bool DoChksum(PerThreadData* perthreaddata, SharedData* shareddata, const RPC* request, RPC* response) {
  const uint8* req_data = request->data; 
  string key = GetStringArg(&req_data);
  string value;

  // Dummy one-entry "cache". There is no mechanism to invaladate old data after a write
  if (perthreaddata->cached_key == key) {
    // Hit
    ////fprintf(stdout, "%s hit\n", key.c_str());
    char chksumbuf[16];	// Only 9 used
    PutStringRPC(string(WeirdChecksum(perthreaddata->cached_value.c_str(), chksumbuf), 8), response);
    WaitMsec(wait_msec);
    return true;
  }

  {
    SpinLock sp(&shareddata->lockandhist);
    // Open key on disk, if any. read/close
    string fname = shareddata->directory + "/" + key;

    uint8* rawptr;
    uint8* ptr = AllocPageAligned(kMaxValueSize, &rawptr);
    bool ok;
    int n;
   kutrace::mark_b("disk");
    if (direct) {
      ok = DirectRead(fname, ptr, kMaxValueSize, &n);
    } else {
      ok = BufferedRead(fname, ptr, kMaxValueSize, &n);
    }
   kutrace::mark_b("/disk");
    if (!ok) {
      response->header->status = FailStatus;	// Let the caller know key wasn't there
      free(rawptr);
      return true;
    }
    // Cache the <key, value> pair
    perthreaddata->cached_key = key;
    perthreaddata->cached_value = string((char*)ptr, n);

    char chksumbuf[16];	// Only 9 used
    PutStringRPC(string(Checksum((char*)ptr, chksumbuf), 8), response);
    free(rawptr);

    WaitMsec(wait_msec);
  }

  return true;
}

// Write <key, value> from request arguments
// No result data
bool DoWrite(SharedData* shareddata, const RPC* request, RPC* response) {
  const uint8* req_data = request->data; 
  string key = GetStringArg(&req_data);
  string value = GetStringArg(&req_data);
  {
    SpinLock sp(&shareddata->lockandhist);
    string fname = shareddata->directory + "/" + key;
    // fprintf(stdout, "  \"%s\"\n", fname.c_str());

    // Check limited length
    if (kMaxValueSize <= value.size()) {
      response->header->status = FailStatus;	// Let the caller know value was too long
      return true;
    }

    bool ok;
    int n;
    if (direct) {
      // Direct has to be multiple of 4KB and aligned. Round down to 4KB
      int valuesize4k = value.size() & ~kPageSizeMask;
      uint8* rawptr;
      uint8* ptr = AllocPageAligned(valuesize4k, &rawptr);
      memcpy(ptr, value.data(), valuesize4k);
      ok = DirectWrite(fname, ptr, valuesize4k, &n);
      free(rawptr);
    } else {
      ok = BufferedWrite(fname, (const uint8*)value.data(), value.size(), &n);
    }
    if (!ok) {
      response->header->status = FailStatus;	// Let the caller know key wasn't there
      return true;
    }

    WaitMsec(wait_msec);
  }

  return true;
}


// Delete <key> from request argument
// No result data
// Return status=fail if key is not found
bool DoDelete(SharedData* shareddata, const RPC* request, RPC* response) {
  const uint8* req_data = request->data; 
  string key = GetStringArg(&req_data);
  {
    SpinLock sp(&shareddata->lockandhist);
    // Open key on disk to get there/notthere
    // Delete 
    string fname = shareddata->directory + "/" + key;
    FILE* f = fopen(fname.c_str(), "r");
    if (f == NULL) {
      response->header->status = FailStatus;	// Let the caller know key open failed
      return true;
    }
    fclose(f);
    int n = remove(fname.c_str());
    // Check for error on remove()
    if (n != 0) {
      response->header->status = FailStatus;	// Let the caller know remove failed
      return true;
    }

    WaitMsec(wait_msec);
  }

  return true;
}


// Return a string of the 32 spinlock-usec histogram values
bool DoStats(SharedData* shareddata, const RPC* request, RPC* response) {
  string result;
  {
    SpinLock sp(&shareddata->lockandhist);
    result.append(StringPrintf("Lock acquire: "));
    for (int i = 0; i < 32; ++i) {
      result.append(StringPrintf("%d ", shareddata->lockandhist.hist[i]));
      if ((i % 10) == 9) {result.append("  ");}
    }
    PutStringRPC(result, response);

    WaitMsec(wait_msec);
  }

  return true;
}


//====
//====

// Helper for deleting all files in a directory
// Return 0 on sucess
int fn(const char *fpath, const struct stat *sb,
       int typeflag, struct FTW *ftwbuf) {
  // fprintf(stdout, "  fn(%s)\n", fpath);
  // Do not delete the top-level directory itself
  if (ftwbuf->level == 0) {return 0;}
  // Check if stat() call failed on fpath
  if (typeflag == FTW_NS) {return 1;}
  int n = remove(fpath);
  // Check if remove() failed
  if (n != 0) {return 1;}
  return 0;
}


// Erase all <key, value> pairs
// No result data
bool DoReset(SharedData* shareddata, const RPC* request, RPC* response) {
  {
    SpinLock sp(&shareddata->lockandhist);
    // delete all 
    int errors = nftw(shareddata->directory.c_str(), fn, 2, FTW_DEPTH);
    if (errors != 0) {
      response->header->status = FailStatus;	// Let the caller know delete failed
    }

    WaitMsec(wait_msec);
  }

  return true;
}


// Create the repsonse to a quit request
// Returns false on any error, with response status indicating why
bool DoQuit(SharedData* shareddata, const RPC* request, RPC* response) {
  return true;
}

// Create the repsonse showing an erroneous request
// Returns false on any error, with response status indicating why
bool DoError(SharedData* shareddata, const RPC* request, RPC* response) {
  // We just send the data back unchanged
  CopyRPCData(request, response);		
  response->header->status = FailStatus;
  return false;
}

// Add a name of type n, value number, to the trace
void AddName(uint64 n, uint64 number, u64 bytelen, const char* name) {
  u64 temp[8];		// Buffer for name entry
  if (bytelen > 55) {bytelen = 55;}
  u64 wordlen = 1 + ((bytelen + 7) / 8);
  // Build the initial word
  u64 n_with_length = n + (wordlen * 16);
  //             T             N                       ARG
  temp[0] = (CLU(0) << 44) | (n_with_length << 32) | (number);
  memset((char*)&temp[1], 0, 7 * sizeof(u64));
  memcpy((char*)&temp[1], name, bytelen);
  kutrace::DoControl(KUTRACE_CMD_INSERTN, (u64)&temp[0]);
}

// Open a TCP/IP socket, bind it to given port, then listen, etc.
// Outer loop: listen, accept, read/write until closed
//   Inner loop: read() until socket is closed or Quit
// Returns true when Quit message is received
void* SocketLoop(void* arg) {
  PerThreadData* perthreaddata = reinterpret_cast<PerThreadData*>(arg);
  SharedData* shareddata = perthreaddata->shareddata;
  int sockfd = OpenSocket(perthreaddata->portnum);

  // Outer loop: listen, accept, read/write*, close connection
  for (;;) {
    if (stopping) {break;}

    bool ok = true;
    uint32 client_ip;
    uint16 client_port;
    int acceptsockfd = ConnectToClient(sockfd, &client_ip, &client_port);

    int optval = 1;
    setsockopt(acceptsockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(int));
    setsockopt(acceptsockfd, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(int));

    // Inner loop: read/process/write RPCs
    for (;;) {
      if (stopping) {break;}

      RPC request;
      RPC response;

      ok &= ReceiveRequest(acceptsockfd, &request);
      if (!ok) {break;}		// Most likely, client dropped the connection
 
      // Process request
      request.header->req_rcv_timestamp = GetUsec();
      request.header->client_ip = client_ip;
      request.header->client_port = client_port;
      request.header->type = ReqRcvType;
  
      // Trace the incoming RPC request
      // RPCid is pseudo-random 32 bits, but never zero. 
      // If low 16 bits are zero, use high 16 bits.
      uint32 tempid;
      tempid = request.header->rpcid & 0xffff;
      if(tempid == 0) {tempid = (request.header->rpcid >> 16) & 0xffff;}
      // Add method name to the trace
      AddName(KUTRACE_METHODNAME, tempid, 8, request.header->method);
      // Add RPC request to the trace
      uint64 entry_count = kutrace::addevent(KUTRACE_RPCIDREQ, tempid);

      LogRPC(shareddata->logfile, &request);
      if (verbose) {
        fprintf(stdout, "server_mystery21: ReceiveRequest:   "); 
        PrintRPC(stdout, &request);
      }
      if (verbose_data) {
        // Send method, key, value to stdout
        const uint8* req_data = request.data; 
        const uint8* req_data_limit = request.data + request.datalen;
        fprintf(stdout, "%s ", request.header->method);
        if (req_data < req_data_limit) {
          string key = GetStringArg(&req_data);
          fprintf(stdout, "%s ", key.c_str()); 
        }
        if (req_data < req_data_limit) {
          string value = GetStringArg(&req_data);
          fprintf(stdout, "%s ", value.c_str()); 
        }
        fprintf(stdout, "\n");
      }


      // Process response
      CopyRPCHeader(&request, &response);		
      response.data = NULL;
      response.datalen = 0;
      RPCHeader* hdr = response.header;
      hdr->type = RespSendType;
      hdr->status = SuccessStatus;

      kutrace::mark_a(hdr->method);

      if (strcmp(hdr->method, "ping") == 0) {
        ok &= DoPing(shareddata, &request, &response);
      } else if (strcmp(hdr->method, "read") == 0) {
        ok &= DoRead(perthreaddata, shareddata, &request, &response);
      } else if (strcmp(hdr->method, "chksum") == 0) {
        ok &= DoChksum(perthreaddata, shareddata, &request, &response);
      } else if (strcmp(hdr->method, "write") == 0) {
        ok &= DoWrite(shareddata, &request, &response);
      } else if (strcmp(hdr->method, "delete") == 0) {
        ok &= DoDelete(shareddata, &request, &response);
      } else if (strcmp(hdr->method, "stats") == 0) {
        ok &= DoStats(shareddata, &request, &response);
      } else if (strcmp(hdr->method, "reset") == 0) {
        ok &= DoReset(shareddata, &request, &response);
      } else if (strcmp(hdr->method, "quit") == 0) {
        ok &= DoQuit(shareddata, &request, &response); 
        stopping = true;
      } else {
        ok &= DoError(shareddata, &request, &response);
      }

      // Stop tracing the incoming RPC request
      kutrace::addevent(KUTRACE_RPCIDREQ, 0);


      // Send response
      hdr->lglen2 = TenLg(response.datalen);
      hdr->resp_send_timestamp = GetUsec();
      hdr->type = RespSendType;
      LogRPC(shareddata->logfile, &response);
      if (verbose) {
        fprintf(stdout, "server_mystery21: SendResponse:     "); 
        PrintRPC(stdout, &response);
      }

      // Trace the outgoing RPC response
      // RPCid is pseudo-random 32 bits, but never zero. 
      // If low 16 bits are zero, send high 16 bits.
      tempid = response.header->rpcid & 0xffff;
      if(tempid == 0) {tempid = (response.header->rpcid >> 16) & 0xffff;}
      kutrace::addevent(KUTRACE_RPCIDRESP, tempid);

      ok &= SendResponse(acceptsockfd, &response);

      FreeRPC(&request);
      FreeRPC(&response);

      // Stop tracing the outgoing RPC response
      kutrace::addevent(KUTRACE_RPCIDRESP, 0);
 
      if (!ok) {break;}		// Most likely, client dropped the connection
    }

    // Connection was closed -- go back around and wait for another connection
    close(acceptsockfd);
 }

  close(sockfd);
  return NULL;
}

void Usage() {
  fprintf(stderr, 
          "Usage: server_mystery21 directory "
          "[portnumber] [num_ports] [-verbose] [-direct] [-data] [-wait msec]\n");
  exit(EXIT_FAILURE);
}


// Just call our little server loop
int main (int argc, const char** argv) {
  if (argc < 2) {Usage();}
  int base_port = -1;
  int num_ports = -1;
  const char* directory = argv[1];
printf("directory = %s\n", directory);

  for (int i = 2; i < argc; ++i) {
    if (strcmp(argv[i], "-verbose") == 0) {
      verbose = true;
    } else if (strcmp(argv[i], "-direct") == 0) {
      direct = true;
    } else if (strcmp(argv[i], "-data") == 0) {
      verbose_data = true;
    } else if ((strcmp(argv[i], "-wait") == 0) && (i < (argc - 1))) {
      wait_msec = atoi(argv[i + 1]);
      ++i;
    } else if (argv[i][0] != '-') {
      // We have a number
      if (base_port < 0) {base_port = atoi(argv[i]);}
      else if (num_ports < 0) {num_ports = atoi(argv[i]);}
    } else {
      Usage();
    }
  }
  // Apply defaults
  if (base_port < 0) {base_port = 12345;}
  if (num_ports < 0) {num_ports = 4;}

  CalibrateCycleCounter();

  // Set up the shared data
  SharedData shareddata;
  ////memset(&shareddata, 0, sizeof(SharedData));
  shareddata.directory = string(directory);
  shareddata.logfilename = MakeLogFileName(argv[0]);
  shareddata.logfile = OpenLogFileOrDie(shareddata.logfilename);
  shareddata.key_value.clear();

  // Create our "database" directory if not already there
  struct stat st = {0};
  if (stat(shareddata.directory.c_str(), &st) == -1) {
    mkdir(shareddata.directory.c_str(), 0700);
  }
printf("directory is %s\n", shareddata.directory.c_str());


  // Set up N ports, N in [1..4]
  // This leaks memory for the small number of PerThreadData structures, but 
  // all we do next is terminate
  fprintf(stderr, "\n");
  for (int n = 0; n < num_ports; ++n) {
    // Allocate a per-thread data structure and fill it in
    PerThreadData* perthreaddata = new PerThreadData;
    perthreaddata->cached_key.clear();		// dummy cache
    perthreaddata->cached_value.clear();	// dummy cache
    perthreaddata->portnum = base_port + n;
    perthreaddata->shareddata = &shareddata;
  
    // Launch a pthread to listen on that port
    // Create independent threads each of which will execute function SocketLoop
    pthread_t thread; 
    fprintf(stderr, "server_mystery21: launching a thread to listen on port %d\n", 
            perthreaddata->portnum);
    int iret = pthread_create( &thread, NULL, SocketLoop, (void*) perthreaddata);
    if (iret != 0) {Error("pthread_create()", iret);}
  }

  int total_seconds = 0;
  while (!stopping) {
    sleep(2);	// Poll every 2 seconds
    total_seconds += 2;
    if (total_seconds >= kMaxRunTimeSeconds) {
      fprintf(stderr, 
              "server_mystery21: timed out after %d minutes (safety move) ...\n", 
              kMaxRunTimeSeconds / 60);
      stopping = true;
    }
    if (stopping) {break;}
  }

  // Do not clear. User must explicitly send reset command
  // Clear the database files on exit. Ignore errors
  //// nftw(shareddata.directory.c_str(), fn, 2, FTW_DEPTH);

  fclose(shareddata.logfile);
  fprintf(stderr, "  %s written\n", shareddata.logfilename);

  exit(EXIT_SUCCESS);
}

