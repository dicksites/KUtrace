// Simple RPC header dclab_rpc.h
// This defines an RPC header and the routines to manipulate it.
// Our dclab client-server routines will use this for all RPC messages
//
// Copyright 2021 Richard L. Sites


#ifndef __DCLAB_RPC_H__
#define __DCLAB_RPC_H__

#include <stdio.h>
#include <string>
#include "basetypes.h"

using std::string;

static const uint32 kMarkerSignature = 3519354853u;  	// Arbitrary unlikely constant
static const int kMaxRPCHeaderLength = (4 * 1024) - 1;
static const int kMaxRPCDataLength = (16 * 1024 * 1024) - 1;

enum RPCType {
  ReqSendType = 0,
  ReqRcvType,
  RespSendType,
  RespRcvType,
  TextType,
  NumType	// must be last 
};

enum RPCStatus {
  SuccessStatus = 0,
  FailStatus,
  TooBusyStatus,
  NumStatus	// must be last
};

// Padded to 8 characters for printing
static const char* const kRPCTypeName[] = {
  "ReqSend ", "ReqRcv  ", "RespSend", "RespRcv ", "Text    "
};

// Padded to 8 characters for printing
static const char* const kRPCStatusName[] = {
  "Success ", "Fail    ",  "TooBusy ", 
};

// Struct transmitted on the wire
// We completely ignore endianness issues here
typedef struct {
  // Marker for our RPC messages. One message may be bigger than one packet
  // All our messages are aligned multiples of four bytes, so scanning for 
  // a marker only has to look at aligned words.
  // The marker is designed for quick detection/rejection in any packet --
  //  First word is unlikely bit pattern so non-marker quickly fails detection
  //  Second word has 20 high-order zeros in marker, low 12 bits nonzero
  //  Third word has 12 high-order zeros in marker
  //  Fourth word is simple checksum of previous three and again is unlikely bit pattern
  uint32 signature;	// Always kMarkerSignature
  uint32 headerlen;
  uint32 datalen;
  uint32 checksum;	// = signature + ((headerlen << 20) ^ datalen)
  // 16 bytes
} RPCMarker;

// Struct transmitted on the wire
// We completely ignore endianness issues here
typedef struct {
  // rpcid is at the front so that kernel TCP Patches can find it easily
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
  uint8 lglen1;         // 10 * lg(request data length in bytes)
  uint8 lglen2;         // 10 * lg(response data length in bytes)
  uint16 type;		// request or response, client-side or server-side, etc.
  // 16 bytes

  char method[8];
  //  8 bytes

  uint32 status;	// 0 = success, other = error code
  uint32 pad;		// Sent as zero. Makes data 8B aligned
  // 72 bytes to here

  uint8 data[0];	// [B2] byte length is in marker above
                        // [B3] off the end of data
} RPCHeader;

// Struct just in memory
// An RPC is represented in memory as three pieces: marker, header and data.
// The RPC marker on the wire specifies the lengths of the other two, which are  
//   copied into the RPC struct for convenience.
// The RPC header specifies timestamps and which machine/method is being called,
// while the data is an arbitrary byte stream of arguments or results for the call.
typedef struct {
  RPCHeader* header;
  uint8* data;
  int32 headerlen;
  int32 datalen;
} RPC;

// The main transmission routines
bool ReadExactlyLenBytes(int sockfd, uint8* buffer, int32 len);
bool ReadOneRPC(int sockfd, RPC* rpc, uint32* hash32);
bool SendOneRPC(int sockfd, const RPC* rpc, uint32* hash32);

// Some utility routines
uint32 MarkerChecksum(const RPCMarker* marker);
bool ValidMarker(const RPCMarker* marker);
void ErrorBadMarker(const RPCMarker* marker);

// Convert uint32 to single-byte 10 * lg(x)
uint8 TenLg(uint32 x);

// Convert 10 * lg(x) back into x
uint64 TenPow(uint8 xlg);

// Copy an RPC, copying all sub-pieces
void CopyRPC(const RPC* srcrpc, RPC* dstrpc);

// Copy an RPC, copying header sub-piece, leaving dst data unchanged
void CopyRPCHeader(const RPC* srcrpc, RPC* dstrpc);

// Copy an RPC, copying data sub-piece, leaving dst header unchanged
void CopyRPCData(const RPC* srcrpc, RPC* dstrpc);

// Free the header and data previously allocated
bool FreeRPC(RPC* rpc);

// Free just the data previously allocated
bool FreeRPCDataOnly(RPC* rpc);

// Our simple delimited strings on the wire have a 4-byte length on the front
// We completely ignore endianness issues here
// Extract a delimited string from RPC data: length, string
string GetStringArg(const uint8** arg);

// Insert a delimited buffer into RPC data: length, string
void PutStringRPC(const char* str, int strlen, RPC* rpc);

// Insert two delimited buffers into RPC data: length, string, length, string
void PutStringRPC2(const char* str1, int str1len, const char* str2, int str2len, RPC* rpc);

// Insert a delimited string into RPC data: length, string
void PutStringRPC(const string& str, RPC* rpc);

// Insert two delimited strings into RPC data: length, string, length, string
void PutStringRPC2(const string& str1, const string& str2, RPC* rpc);

// Fold 32-bit rpcid to 16-bit one
// 32-bit rpcid is never zero. If low bits are zero, use high bits
inline uint32 rpcid32_to_rpcid16(uint32 rpcid) {
  uint32 tempid = rpcid & 0xffff;
  return (tempid == 0) ? (rpcid >> 16) : tempid;
}

// Fold 32-bit packet hash to 16-bit one
inline uint32 hash32_to_hash16(uint32 hash32) {
  return (hash32 ^ (hash32 >> 16)) & 0xFFFF;
}

#endif	// __DCLAB_RPC_H__



