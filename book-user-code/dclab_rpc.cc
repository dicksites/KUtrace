// dclab_rpc.cc
// Copyright 2021 Richard L. Sites

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <string>

#include "basetypes.h"
#include "dclab_log.h"
#include "dclab_rpc.h"
#include "kutrace_lib.h"

using std::string;

// 10 * lg(x) rounded to nearest integer, with lg(zero) mapped to 0
static const uint8 kTenLgTable[256] = {
 0,  0, 10, 16, 20, 23, 26, 28, 30, 32, 33, 35, 36, 37, 38, 39, 
40, 41, 42, 42, 43, 44, 45, 45, 46, 46, 47, 48, 48, 49, 49, 50, 
50, 50, 51, 51, 52, 52, 52, 53, 53, 54, 54, 54, 55, 55, 55, 56, 
56, 56, 56, 57, 57, 57, 58, 58, 58, 58, 59, 59, 59, 59, 60, 60, 
60, 60, 60, 61, 61, 61, 61, 61, 62, 62, 62, 62, 62, 63, 63, 63, 
63, 63, 64, 64, 64, 64, 64, 64, 65, 65, 65, 65, 65, 65, 66, 66, 
66, 66, 66, 66, 66, 67, 67, 67, 67, 67, 67, 67, 68, 68, 68, 68, 
68, 68, 68, 68, 69, 69, 69, 69, 69, 69, 69, 69, 70, 70, 70, 70, 
70, 70, 70, 70, 70, 71, 71, 71, 71, 71, 71, 71, 71, 71, 71, 72, 
72, 72, 72, 72, 72, 72, 72, 72, 72, 73, 73, 73, 73, 73, 73, 73, 
73, 73, 73, 73, 74, 74, 74, 74, 74, 74, 74, 74, 74, 74, 74, 75, 
75, 75, 75, 75, 75, 75, 75, 75, 75, 75, 75, 75, 76, 76, 76, 76, 
76, 76, 76, 76, 76, 76, 76, 76, 76, 77, 77, 77, 77, 77, 77, 77, 
77, 77, 77, 77, 77, 77, 77, 77, 78, 78, 78, 78, 78, 78, 78, 78, 
78, 78, 78, 78, 78, 78, 78, 79, 79, 79, 79, 79, 79, 79, 79, 79, 
79, 79, 79, 79, 79, 79, 79, 79, 80, 80, 80, 80, 80, 80, 80, 80, 
};

// 2**0.0 through 2** 0.9
static const double kPowerTwoTenths[10] = {
  1.0000, 1.0718, 1.1487, 1.2311, 1.3195, 
  1.4142, 1.5157, 1.6245, 1.7411, 1.8661
};

// Make the 16-byte marker for an RPC on the wire
void MakeRPCMarker(const RPC* rpc, RPCMarker* marker) {
  marker->signature = kMarkerSignature;
  marker->headerlen = rpc->headerlen;
  marker->datalen = rpc->datalen;
  marker->checksum = MarkerChecksum(marker);
}

//
// The main transmission routines
//

// For len = 0, returns true without doing a read and without using the buffer pointer
bool ReadExactlyLenBytes(int sockfd, uint8* buffer, int32 len) {
  uint8* nextbyte = buffer;
  int32 neededbytes = len;
  while (neededbytes > 0) {
    // Socket data may come in pieces of a few bytes each
    // n = -1 indicates an error. n = 0 indicates closed socket ??
    int n = read(sockfd, nextbyte, neededbytes);
    // if (n == 0) {ErrorNoFail("ReadExactly got 0 bytes"); return false;}
    if (n == 0) {return false;}	// Zero bytes is normal at end
    if (n <= 0) {Error("ReadExactly error"); return false;}
    nextbyte += n;
    neededbytes -= n;
  }
  return true;
}

// Read next incoming RPC request/response.
// Will block until all bytes arrive or read fails (e.g. connection drops)
//
// If successful, 
// sets header to point to allocated buffer and sets headerlen to match,
// sets data to point to another allocated buffer and sets datalen to match. 
// Caller must do delete[] on both. 
//
// Returns true if a good request is found, false on error of any kind.
// Later: if synchronizing on the marker is a problem, try to recover.
//
// Also calculate the same xor hash over the first 32 bytes of the message that 
// the kernel does, for tracking message first packet vs. receipt here in user code
bool ReadOneRPC(int sockfd, RPC* rpc, uint32* hash32) {
  rpc->header = NULL;
  rpc->headerlen = 0;
  rpc->data = NULL;
  rpc->datalen = 0;

  // Read the RPC marker
  RPCMarker marker;
  uint8* markerbuffer = reinterpret_cast<uint8*>(&marker);
  bool ok = true;
  if (hash32 != NULL) {*hash32 = 0;}

  ok &= ReadExactlyLenBytes(sockfd, markerbuffer, sizeof(RPCMarker));

  // If we read zero bytes because no command arrived, ok will be set false.
  // Client likely closed the socket, so we bail
  if (!ok) {return ok;}
 
  // We now have bytes for a complete marker
  if (!ValidMarker(&marker)) {ErrorBadMarker(&marker);}

  // Read the RPCheader
  if (marker.headerlen > 0) {
    uint8* hdr = new uint8[marker.headerlen];  
    rpc->header = reinterpret_cast<RPCHeader*>(hdr);
    rpc->headerlen = marker.headerlen;
    ok &= ReadExactlyLenBytes(sockfd, hdr, rpc->headerlen);
  } 

  // We now have a complete valid marker; gather the rest of the RPC bytes
  uint32 packet_hash = 0;
  for (int i = 0; i < 4; ++i) {packet_hash ^= (reinterpret_cast<const uint32*>(&marker))[i];}
  for (int i = 0; i < 4; ++i) {packet_hash ^= (reinterpret_cast<const uint32*>(rpc->header))[i];}
  if (hash32 != NULL) {*hash32 = packet_hash;}

  // 2021 Add user-mode receipt with RPCID and 16-bit message first packet hash to trace
  //      RPCID in to 16 bits of arg, hash16 in bottom
  // No, back to full 32-bit hash
  ////uint32 rpcid16 = rpcid32_to_rpcid16(rpc->header->rpcid);
  ////uint32 hash16 = hash32_to_hash16(packet_hash);
  ////kutrace::addevent(KUTRACE_RX_USER, (rpcid16 << 16) | hash16);
  kutrace::addevent(KUTRACE_RX_USER, packet_hash);

  // Read the data
  if (marker.datalen > 0) {
    rpc->data = new uint8[marker.datalen];
    rpc->datalen = marker.datalen;
    ok &= ReadExactlyLenBytes(sockfd, rpc->data, rpc->datalen);
  } 

  return ok;
}

// Send one RPC over the wire: marker, header, data
bool SendOneRPC(int sockfd, const RPC* rpc, uint32* hash32) {
  int iret;
  RPCMarker mymarker;
  MakeRPCMarker(rpc, &mymarker);

  uint32 packet_hash = 0;
  for (int i = 0; i < 4; ++i) {packet_hash ^= (reinterpret_cast<const uint32*>(&mymarker))[i];}
  for (int i = 0; i < 4; ++i) {packet_hash ^= (reinterpret_cast<const uint32*>(rpc->header))[i];}
  if (hash32 != NULL) {*hash32 = packet_hash;}

  // 2021 Add user-mode send with RPCID and 16-bit message first packet hash to trace
  //      RPCID in to 16 bits of arg, hash16 in bottom
  // No, back to full 32-bit hash
  ////uint32 rpcid16 = rpcid32_to_rpcid16(rpc->header->rpcid);
  ////uint32 hash16 = hash32_to_hash16(packet_hash);
  ////kutrace::addevent(KUTRACE_TX_USER, (rpcid16 << 16) | hash16);
  kutrace::addevent(KUTRACE_TX_USER, packet_hash);
  
#if 1
  // Make a single message to transmit
  string msg;
  msg.reserve(sizeof(RPCMarker) + rpc->headerlen + rpc->datalen);
  msg.append(reinterpret_cast<const char*>(&mymarker), sizeof(RPCMarker));
  msg.append(reinterpret_cast<const char*>(rpc->header), rpc->headerlen);
  msg.append(reinterpret_cast<const char*>(rpc->data), rpc->datalen);
  iret = write(sockfd, msg.data(), msg.length());
  if (iret < 0) {Error("write message");}
  return true;

#else

  iret = write(sockfd, &mymarker, sizeof(RPCMarker)); 
  if (iret < 0) {Error("write marker");}

  if (rpc->headerlen > 0) {
    iret= write(sockfd, rpc->header, rpc->headerlen);
    if (iret < 0) {Error("write header");}
  } 
  if (rpc->datalen > 0) {
    iret = write(sockfd, rpc->data, rpc->datalen);
    if (iret < 0) {Error("write data");}
  } 
#endif

  return true;
}


//
// Some utility routines
//

uint32 MarkerChecksum(const RPCMarker* marker) {
  return marker->signature + ((marker->headerlen << 20) ^ (marker->datalen));
}

// Client and server both deal in little-endian byte streams, so no ntoh* needed
bool ValidMarker(const RPCMarker* marker) {
  if (marker->signature != kMarkerSignature ) {return false;}
  if (marker->headerlen > kMaxRPCHeaderLength) {return false;}
  if (marker->datalen > kMaxRPCDataLength) {return false;}
  if (marker->checksum != MarkerChecksum(marker)) {return false;}
  return true;
}

void ErrorBadMarker(const RPCMarker* marker) {
  const uint8* umarker = reinterpret_cast<const uint8*>(marker);
  fprintf(stderr, "Invalid marker received: ");
  // Print what we got to make it possible to understand what went wrong
  for (int i = 0; i < 16; ++i) {
    fprintf(stderr, "%02x", umarker[i]); 
    if ((i & 3) == 3) {fprintf(stderr, " ");}
  }
  fprintf(stderr, "\n");
  exit(EXIT_FAILURE);
}


// Convert uint32 to single-byte 10 * lg(x)
uint8 TenLg(uint32 x) {
  if (x == 0) {return 0;}
  if (x >= 47453132) {return 255;}
  int32 floorlg = FloorLg(x);	// returns 7 for 255, 8 for 256
  uint8 tenlg = 0;
  uint32 local_x = x;
  if (floorlg > 7) {
    local_x >>= (floorlg - 7);
    tenlg += (floorlg - 7) * 10;
  }
  tenlg += kTenLgTable[local_x];
//fprintf(stderr, "TenLg %d = %d %d %d %d\n", x, floorlg, local_x, kTenLgTable[local_x],tenlg);
  return tenlg;
}

// Convert ten * lg(x) back into x
uint64 TenPow(uint8 xlg) {
  int powertwo = xlg / 10;
  int fraction = xlg % 10;
  uint64 retval = 1llu << powertwo;
  retval = (retval * kPowerTwoTenths[fraction]) + 0.5;
  return retval;
}


// Copy an RPC, copying sub-pieces
void CopyRPC(const RPC* srcrpc, RPC* dstrpc) {
  dstrpc->header = new RPCHeader;
  dstrpc->headerlen = sizeof(RPCHeader);
  memcpy(dstrpc->header, srcrpc->header, sizeof(RPCHeader));
  dstrpc->data = new uint8[srcrpc->datalen];
  dstrpc->datalen = srcrpc->datalen;
  memcpy(dstrpc->data, srcrpc->data, srcrpc->datalen);
}

// Copy an RPC, copying header sub-piece, leaving dst data unchanged
void CopyRPCHeader(const RPC* srcrpc, RPC* dstrpc) {
  dstrpc->header = new RPCHeader;
  dstrpc->headerlen = sizeof(RPCHeader);
  memcpy(dstrpc->header, srcrpc->header, sizeof(RPCHeader));
}

// Copy an RPC, copying data sub-piece, leaving dst header unchanged
void CopyRPCData(const RPC* srcrpc, RPC* dstrpc) {
  dstrpc->data = new uint8[srcrpc->datalen];
  dstrpc->datalen = srcrpc->datalen;
  memcpy(dstrpc->data, srcrpc->data, srcrpc->datalen);
}

// Free the header and data previously allocated
bool FreeRPC(RPC* rpc) {
  delete rpc->header;
  rpc->header = NULL;
  rpc->headerlen = 0;
  delete[] rpc->data;
  rpc->data = NULL;
  rpc->datalen = 0;
  return true;
}

// Free just the data previously allocated
bool FreeRPCDataOnly(RPC* rpc) {
  rpc->header = NULL;
  rpc->headerlen = 0;
  delete[] rpc->data;
  rpc->data = NULL;
  rpc->datalen = 0;
  return true;
}


// Our simple delimited strings on the wire have a 4-byte length on the front
// We completely ignore endianness issues here
// Extract a delimited string from RPC data: length, string
// arg points to a uint32 N followed by N bytes
// Return the N bytes as a string and update arg to point to the following byte
string GetStringArg(const uint8** arg) {
  uint32 len = *reinterpret_cast<const uint32*>(*arg);
  *arg += sizeof(uint32);
  const char* s = reinterpret_cast<const char*>(*arg);
  *arg += len;
  return string(s, len);
}

// Insert a delimited buffer into RPC data: length, string
void PutStringRPC(const char* str, int strlen, RPC* rpc) {
  uint32 len = strlen;
  rpc->datalen = sizeof(uint32) + len;
  rpc->data = new uint8[rpc->datalen];
  uint8* d = rpc->data;

  // Put in length, then string
  *reinterpret_cast<uint32*>(d) = len;
  d += sizeof(uint32);
  memcpy(reinterpret_cast<char*>(d), str, len);
}

// Insert two delimited buffers into RPC data: length, string, length, string
void PutStringRPC2(const char* str1, int str1len, const char* str2, int str2len, RPC* rpc) {
  uint32 len1 = str1len;
  uint32 len2 = str2len;
  rpc->datalen = 2 * sizeof(uint32) + len1 + len2;
  rpc->data = new uint8[rpc->datalen];
  uint8* d = rpc->data;

  // Put in length, then str1
  *reinterpret_cast<uint32*>(d) = len1;
  d += sizeof(uint32);
  memcpy(reinterpret_cast<char*>(d), str1, len1);
  d += len1;

  // Put in length, then str2
  *reinterpret_cast<uint32*>(d) = len2;         // May well be unaligned
  d += sizeof(uint32);
  memcpy(reinterpret_cast<char*>(d), str2, len2);
}


// Insert a delimited string into RPC data: length, string
void PutStringRPC(const string& str, RPC* rpc) {
  uint32 len = str.size();
  rpc->datalen = sizeof(uint32) + len;
  rpc->data = new uint8[rpc->datalen];
  uint8* d = rpc->data;

  // Put in length, then string
  *reinterpret_cast<uint32*>(d) = len;
  d += sizeof(uint32);
  memcpy(reinterpret_cast<char*>(d), str.data(), len);
}

// Insert two delimited strings into RPC data: length, string, length, string
void PutStringRPC2(const string& str1, const string& str2, RPC* rpc) {
  uint32 len1 = str1.size();
  uint32 len2 = str2.size();
  rpc->datalen = 2 * sizeof(uint32) + len1 + len2;
  rpc->data = new uint8[rpc->datalen];
  uint8* d = rpc->data;

  // Put in length, then str1
  *reinterpret_cast<uint32*>(d) = len1;
  d += sizeof(uint32);
  memcpy(reinterpret_cast<char*>(d), str1.data(), len1);
  d += len1;

  // Put in length, then str2
  *reinterpret_cast<uint32*>(d) = len2;         // May well be unaligned
  d += sizeof(uint32);
  memcpy(reinterpret_cast<char*>(d), str2.data(), len2);
}













