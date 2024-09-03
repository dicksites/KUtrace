// server4.cc cloned from server2.cc 2018.04.16
// Copyright 2021 Richard L. Sites
//
// compile with g++ -O2 -pthread server4.cc dclab_log.cc dclab_rpc.cc kutrace_lib.cc spinlock.cc -o server4

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>		// read()
#include <netinet/in.h> 
#include <netinet/ip.h> /* superset of previous */
#include <netinet/tcp.h>
#include <sys/socket.h> 
#include <sys/types.h> 
 
#include <map>
#include <string>

#include "basetypes.h"
#include "dclab_log.h"
#include "dclab_rpc.h"
#include "kutrace_lib.h"
#include "spinlock.h"
#include "timecounters.h"

using std::map;
using std::string;


typedef map<string, string> MapKeyValue;

typedef struct {
  LockAndHist lockandhist;
  const char* logfilename;
  FILE* logfile;
  MapKeyValue key_value;
} SharedData;

typedef struct {
  uint16 portnum;
  SharedData* shareddata;
} PerThreadData;

static const int kMaxRunTimeSeconds = 4 * 60;

// Global flags
static bool verbose = false;
static bool verbose_data = false;
static bool stopping = false;	// Any thread can set this true

int OpenSocket(int16 portnum) {
  // Open a TCP/IPv4 socket. 
  // Returns file descriptor if OK, -1 and sets errno if bad
  //fprintf(stderr, "server4: Open server socket\n");
  int sockfd = socket(AF_INET, SOCK_STREAM, 0);  
  if (sockfd < 0) {Error("Socket open");}

  // Bind this socket to a particular TCP/IP port.
  // Construct server address structure first
  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;		// TCP/IPv4
  server_addr.sin_addr.s_addr = INADDR_ANY;	// Find out my IP address
  server_addr.sin_port = htons(portnum);	// host-to-network of short int portnum
 
  //fprintf(stderr, "server4: Bind server socket, port %d\n", portnum);
  int iretbind = bind(sockfd, 
                      reinterpret_cast<struct sockaddr*>(&server_addr), 
                      sizeof(server_addr));
  if (iretbind != 0) {Error("Bind socket");}
  //fprintf(stderr, "server4: Bound server socket %08x:%04x\n", server_addr.sin_addr.s_addr, server_addr.sin_port);

  return sockfd;
}

// Accept a client connection and return the new socket fd
int ConnectToClient(int sockfd, uint32* client_ip, uint16* client_port) {
  // Listen on the bound port for a connection attempt.
  // Allow the default maximum 5 simultaneous attempts (four of which would wait)A
  //fprintf(stderr, "server4: listen server socket\n");
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
  fprintf(stderr, "server4: connection from %s\n", FormatIpPort(*client_ip, *client_port));
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

// Read <key> from request argument
// Set result data to <value>
// Return status=fail and empty data if key is not found
bool DoRead(SharedData* shareddata, const RPC* request, RPC* response) {
  const uint8* req_data = request->data; 
  string key = GetStringArg(&req_data);
  string value;
  {
    SpinLock sp(&shareddata->lockandhist);
    MapKeyValue::const_iterator it = shareddata->key_value.find(key);
    if (it == shareddata->key_value.end()) {
      response->header->status = FailStatus;	// Let the caller know key wasn't there
    } else {
      PutStringRPC(it->second, response);
    } 
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
    shareddata->key_value[key] = value;
  }
  return true;
}

// Throw data away as quickly as possible (allow client outbound saturation)
// No result data
bool DoSink(SharedData* shareddata, const RPC* request, RPC* response) {
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
    MapKeyValue::iterator it = shareddata->key_value.find(key);
    if (it == shareddata->key_value.end()) {
      response->header->status = FailStatus;	// Let tthe caller know key wasn't there
    } else {
      shareddata->key_value.erase(it);
    } 
  }
  return true;
}


// Return a string of the 32 spinlock-usec histogram values
bool DoStats(SharedData* shareddata, const RPC* request, RPC* response) {
  string result;
  {
    SpinLock sp(&shareddata->lockandhist);
    result.append(StringPrintf("Stats: "));
    for (int i = 0; i < 32; ++i) {
      result.append(StringPrintf("%d ", shareddata->lockandhist.hist[i]));
    }
    PutStringRPC(result, response);
  }
  return true;
}


// Erase all <key, value> pairs
// No result data
bool DoReset(SharedData* shareddata, const RPC* request, RPC* response) {
  {
    SpinLock sp(&shareddata->lockandhist);
    shareddata->key_value.clear();
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

// The working-on-RPC events KUTRACE_RPCIDREQ and KUTRACE_RPCIDRESP have this 
// format:
// +-------------------+-----------+---------------+-------+-------+
// | timestamp 2       | event     |     lglen8    |     RPCid     | (2)
// +-------------------+-----------+---------------+-------+-------+
//          20              12         8       8           16 

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
      request.header->req_rcv_timestamp = GetUsec();		// T2
      request.header->client_ip = client_ip;
      request.header->client_port = client_port;
      request.header->type = ReqRcvType;
  
      // Trace the incoming RPC request
      // RPCid is pseudo-random 32 bits, but never zero. If low 16 bits are zero, use high 16 bits.
      uint32 tempid = rpcid32_to_rpcid16(request.header->rpcid);
      uint8 lglen8 = request.header->lglen1;	// Request length

      // Upon new request, do these trace entries:
      //  1) Method name w/rpcid
      //  2) RPC REQ w/rpcid and lglen8
      //  3) Hash of first 32 bytes of request msg, to match a packet hash recorded within the kernel

      // rls 2020.08.23 record the method name for each incoming RPC
      kutrace::addname(KUTRACE_METHODNAME, tempid, request.header->method);

      // Start tracing the incoming RPC request
      // We also pack in the 16-bit hash over the first 32 bytes of the packet payload
      kutrace::addevent(KUTRACE_RPCIDREQ, (lglen8 << 16) | tempid);

      if (verbose) {
        fprintf(stdout, "server4: ReceiveRequest:   "); 
        PrintRPC(stdout, &request);
      }
      LogRPC(shareddata->logfile, &request);

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


      // Create response
      CopyRPCHeader(&request, &response);		
      response.data = NULL;
      response.datalen = 0;
      RPCHeader* hdr = response.header;
      hdr->type = RespSendType;
      hdr->status = SuccessStatus;

      // Insert a marker for serving this method request
      kutrace::mark_a(hdr->method);

      // Do the request
      if (strcmp(hdr->method, "ping") == 0) {ok &= DoPing(shareddata, &request, &response);}
      else if (strcmp(hdr->method, "read") == 0) {ok &= DoRead(shareddata, &request, &response);}
      else if (strcmp(hdr->method, "write") == 0) {ok &= DoWrite(shareddata, &request, &response);}
      else if (strcmp(hdr->method, "sink") == 0) {ok &= DoSink(shareddata, &request, &response);}
      else if (strcmp(hdr->method, "delete") == 0) {ok &= DoDelete(shareddata, &request, &response);}
      else if (strcmp(hdr->method, "stats") == 0) {ok &= DoStats(shareddata, &request, &response);}
      else if (strcmp(hdr->method, "reset") == 0) {ok &= DoReset(shareddata, &request, &response);}
      else if (strcmp(hdr->method, "quit") == 0) {
        ok &= DoQuit(shareddata, &request, &response); 
        stopping = true;
      }
      else {ok &= DoError(shareddata, &request, &response);}

      // Stop tracing the RPC request
      kutrace::addevent(KUTRACE_RPCIDREQ, 0);


      // Prepare response
      lglen8 =  TenLg(response.datalen);
      hdr->lglen2 = lglen8;	// Response length
      hdr->resp_send_timestamp = GetUsec();			// T3
      hdr->type = RespSendType;

      // Start tracing response
      kutrace::addevent(KUTRACE_RPCIDRESP, (lglen8 << 16) | tempid);

      if (verbose) {fprintf(stdout, "server4: SendResponse:     "); PrintRPC(stdout, &response);}
      LogRPC(shareddata->logfile, &response);

      // Send response
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
  fprintf(stderr, "Usage: server4 portnumber [num_ports] [-verbose] [-data]\n");
  exit(EXIT_FAILURE);
}


// Just call our little server loop
int main (int argc, const char** argv) {
  int base_port = -1;
  int num_ports = -1;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "-verbose") == 0) {verbose = true;}
    if (strcmp(argv[i], "-data") == 0) {verbose_data = true;}
    else if (argv[i][0] != '-') {
      // We have a number
      if (base_port < 0) {base_port = atoi(argv[i]);}
      else if (num_ports < 0) {num_ports = atoi(argv[i]);}
    }
  }
  // Apply defaults
  if (base_port < 0) {base_port = 12345;}
  if (num_ports < 0) {num_ports = 4;}

  CalibrateCycleCounter();

  // Set up the shared data
  SharedData shareddata;
  memset(&shareddata, 0, sizeof(SharedData));
  shareddata.key_value.clear();
  shareddata.logfilename = MakeLogFileName(argv[0]);
  shareddata.logfile = OpenLogFileOrDie(shareddata.logfilename);
 
  // Set up N ports, N in [1..4]
  // This leaks memory for the small number of PerThreadData structures, but 
  // all we do next is terminate
  fprintf(stderr, "\n");
  for (int n = 0; n < num_ports; ++n) {
    // Allocate a per-thread data structure and fill it in
    PerThreadData* perthreaddata = new PerThreadData;
    perthreaddata->portnum = base_port + n;
    perthreaddata->shareddata = &shareddata;
  
    // Launch a pthread to listen on that port
    // Create independent threads each of which will execute function SocketLoop
    pthread_t thread; 
    fprintf(stderr, "server4: launching a thread to listen on port %d\n", perthreaddata->portnum);
    int iret = pthread_create( &thread, NULL, SocketLoop, (void*) perthreaddata);
    if (iret != 0) {Error("pthread_create()", iret);}
  }

  int total_seconds = 0;
  while (!stopping) {
    sleep(2);	// Poll every 2 seconds
    total_seconds += 2;
    if (total_seconds >= kMaxRunTimeSeconds) {
      fprintf(stderr, 
              "server4: timed out after %d minutes (safety move) ...\n", 
              kMaxRunTimeSeconds / 60);
      stopping = true;
    }
    if (stopping) {break;}
  }

  fclose(shareddata.logfile);
  fprintf(stderr, "%s written\n", shareddata.logfilename);

  exit(EXIT_SUCCESS);
}

