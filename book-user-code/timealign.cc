// timealign.cc
// Little program to align two or more RPC logs
// Copyright 2021 Richard L. Sites
//
// compile with g++ -O2 timealign.cc dclab_log.cc dclab_rpc.cc kutrace_lib.cc -o timealign

#include <stdio.h>
#include <string.h>

#include <map>
#include <string>

#include "basetypes.h"
#include "dclab_log.h"
#include "dclab_rpc.h"

using std::map;
using std::string;

// Assumed Ethernet speed in gigabits per second
static const int64 kGbs = 1;

// Assumed RPC message overhead, in addition to pure data
static const int64 kMsgOverheadBytes = 100;

// Assumed time for missing transmission or server time, in usec
static const int kMissingTime = 2;

static const int kBucketNum = 8;
static const int64 kEmptyBucket = 999999999;

// 2**0.0 through 2** 0.9
static const double kPowerTwoTenths[10] = {
  1.0000, 1.0718, 1.1487, 1.2311, 1.3195, 
  1.4142, 1.5157, 1.6245, 1.7411, 1.8661
};

// This keeps track of one time-offset entry between clocks on two machines
// These ALWAYS refer to mapping server time to client time
typedef struct {
  int64 slop;
  int64 tfrom;
  int64 deltamin;
  int64 deltamax;
} Bucket;

// This records the alignment parameters to map time offsets for one machine pair
//   delta_yi = b + m * (yi - y0)	// The thing to add to yi to get yi'
//   yi' = yi + delta_yi
// To backmap
//   yi' = yi + (b + m * (yi - y0))
//   yi = yi' - (b + m * (yi - y0))
//   delta_yi' = - (b + m * (yi - y0))	// The thing to add to yi' to get yi
//   delta_yi' = - (b + m * ((yi' + delta_yi') - y0))
//   delta_yi' = - (b + m * yi' + m * delta_yi' - m * y0)
//   delta_yi' = - b - m * yi' - m * delta_yi' + m * y0
//   delta_yi' + m * delta_yi' = -b - m * (yi' - y0)
//   delta_yi' * (1 + m) = -b - m * (yi' - y0)
//   delta_yi' = (-b - m * (yi' - y0)) / (1 + m)
//   delta_yi' = -b / (1 + m) - (m  / (1 + m)) * (yi' - y0))
//   yi = yi' - delta_yi'
//
// This could either reflect mapping client to server or server to client, or
// either one to the time of some third machine
typedef struct {
  int64 y0;	// usec since the epoch
  double m;	// slope of clock frequency difference, likely near zero
  double b;	// clock offset at y0, in usec
} Alignment;

// This keeps track of one small array of time offsets for one machine pair,
// plus the calculated time-alignment parameters
typedef struct {
  int64 entrynum;
  int64 bucket_shift;
  bool time_mapping_assigned;	// Final mappings to some base ip assigned
  Bucket buckets[kBucketNum];
  Alignment t14_alignment;	// Map client times to some base ip time (initially identity)
  Alignment t23_alignment;	// Map server times to some base ip time (initially this client)
} BucketStruct;

// This keeps track of arrays of time offsets and alignments for multiple machine pairs
//
// The key is (machine1_ip << 32) | machine2_ip with the smaller
// ip value as machine1 (canonical form)
//
// Time alignments will eventually map all times for all machines to 
// the lowest ip address encountered
//
typedef map<uint64, BucketStruct> BucketMap;
typedef map<uint32, Alignment> IpToAlignment;

static char datebuf[64];

int64 imax(int64 a, int64 b) {return (a >= b) ? a : b;}

const char* datetostr(int64 timeusec) {
  int hr = (timeusec / 3600000000) % 24;
  int min = (timeusec / 60000000) % 60;
  int sec = (timeusec / 1000000) % 60;
  int usec = timeusec % 1000000;
  sprintf(datebuf, "%02d:%02d:%02d.%06d", hr, min, sec, usec);
  return datebuf;
}

void DumpAlignment(FILE* f,  const Alignment* alignment) {
  fprintf(f, "y0 %s offset %5.1fus slope %5.2fus/sec\n", 
          datetostr(alignment->y0), alignment->b, alignment->m * 1000000.0);
}

void DumpAlignments(FILE* f,  const BucketStruct* cur_pair) {
  fprintf(f, "  t14_alignment "); DumpAlignment(f, &cur_pair->t14_alignment);
  fprintf(f, "  t23_alignment "); DumpAlignment(f, &cur_pair->t23_alignment);
}

void InitBuckets(int64 cur_bucket, Bucket* buckets) {
  for (int i = cur_bucket; i < kBucketNum; ++i) {
    buckets[i].slop = kEmptyBucket;
    buckets[i].tfrom = 0;
    buckets[i].deltamin = 0;
    buckets[i].deltamax = 0;
  }
}

void InitAlignment(Alignment* alignment) {
  alignment->y0 = 0.0;
  alignment->m = 0.0;
  alignment->b = 0.0;
}

void InitBucketStruct(BucketStruct* cur_pair) {
  cur_pair->entrynum = 0;
  cur_pair->bucket_shift = 0;
  cur_pair->time_mapping_assigned = false;
  InitBuckets(0, cur_pair->buckets);
  InitAlignment(&cur_pair->t14_alignment);
  InitAlignment(&cur_pair->t23_alignment);
}

void DumpBuckets(FILE* f, const Bucket* buckets) {
  fprintf(f, "\nDumpbuckets\n");
  for (int i = 0; i < kBucketNum; ++i) {
    fprintf(f, "[%d] slop/tfrom/delta %s %lld %lld %lld..%lld = %lld\n", 
            i, datetostr(buckets[i].tfrom),
            buckets[i].slop, buckets[i].tfrom, 
            buckets[i].deltamin, buckets[i].deltamax,
            (buckets[i].deltamin + buckets[i].deltamax) / 2);
  }
}

void DumpBucketStruct(FILE* f, const BucketStruct* cur_pair) {
  fprintf(f, "\nDumpbucketStruct\n");
  fprintf(f, "  entrynum %lld\n", cur_pair->entrynum);
  fprintf(f, "  bucket_shift %lld\n", cur_pair->bucket_shift);
  fprintf(f, "  time_mapping_assigned %d\n", cur_pair->time_mapping_assigned);
  if (1 < cur_pair->entrynum) {
    DumpBuckets(f, cur_pair->buckets);
    DumpAlignments(f, cur_pair);
  }
}

// return 2 * (x/10)
int64 ExpTenths(uint8 x) {
  int64 powertwo = x / 10; 
  int64 fraction = x % 10;
  int64 retval = 1l << powertwo;
  retval *= kPowerTwoTenths[fraction];
  return retval;
}

// Return sec to transmit x bytes at y Gb/s, where 1 Gb/s = 125000000 B/sec
// but we assume we only get about 95% of this for real data, so 120 B/usec
int64 BytesToUsec(int64 x) {
  int64 retval = x * kGbs / 120;
  return retval;
}

int64 RpcMsglglenToUsec(uint8 lglen) {
  return BytesToUsec(ExpTenths(lglen) + kMsgOverheadBytes);
}

// Given alignment parameters x2y, calculate y to x
// See algebra up at the top
void InvertAlignment(const Alignment* xtoy, Alignment* ytox) {
  ytox->y0 = xtoy->y0;
  ytox->m = -xtoy->m / (1.0 + xtoy->m);
  ytox->b = -xtoy->b / (1.0 + xtoy->m);
fprintf(stdout, "  Invert xtoy "); DumpAlignment(stdout, xtoy);
fprintf(stdout, "         ytox "); DumpAlignment(stdout, ytox);
}

// Given alignment parameters x2y, and ytoz, calculate xtoz
// Algebra here:
//  t1' =  t1  + xtoy->m * (t1  - xtoy->y0) + xtoy->b
//  t1'' = t1' + ytoz->m * (t1' - ytoz->y0) + ytoz->b
//
//  t1'' = t1  + (xtoy->m * (t1  - xtoy->y0) + xtoy->b) + 
//         ytoz->m * ((t1  + xtoy->m * (t1  - xtoy->y0) + xtoy->b) - ytoz->y0) + 
//         ytoz->b
//  t1'' = t1  + (xtoy->m * t1 - xtoy->m * xtoy->y0 + xtoy->b) + 
//         ytoz->m * (t1  + xtoy->m * t1 - xtoy->m * xtoy->y0 + xtoy->b - ytoz->y0) + 
//         ytoz->b
//  t1'' = t1  + (xtoy->m * t1 - xtoy->m * xtoy->y0 + xtoy->b) + 
//         (ytoz->m * t1  + ytoz->m * xtoy->m * t1 - ytoz->m * xtoy->m * xtoy->y0 + ytoz->m * xtoy->b - ytoz->m * ytoz->y0) + 
//         ytoz->b
//
// We want
//  t1'' = t1 + xtoz->m * (t1 -  xtoz->y0) + xtoz->b
//         
// So
//  xtoz->m = (xtoy->m + ytoz->m + ytoz->m * xtoy->m)
//  xtoz->y0 = (xtoy->m * xtoy->y0 + ytoz->m * xtoy->m * xtoy->y0 + ytoz->m * ytoz->y0) / xtoz->m
//  xtoz->b = (xtoy->b + ytoz->m * xtoy->b + ytoz->b)
//
// Update-in-place OK if xtoz is one of the other two mappings
void MergeAlignment(const Alignment* xtoy, const Alignment* ytoz, Alignment* xtoz) {
  Alignment temp;
  temp.m = xtoy->m + ytoz->m + ytoz->m * xtoy->m;
  if (temp.m == 0.0) {
    temp.y0 = 0;
  } else {
    temp.y0 = (xtoy->m * xtoy->y0 + ytoz->m * xtoy->m * xtoy->y0 + ytoz->m * ytoz->y0) / temp.m;
  }
  temp.b = xtoy->b + ytoz->b + ytoz->m * xtoy->b;
fprintf(stdout, "  Merge xtoy "); DumpAlignment(stdout, xtoy);
fprintf(stdout, "        ytoz "); DumpAlignment(stdout, ytoz);
fprintf(stdout, "        xtoz "); DumpAlignment(stdout, &temp);
  *xtoz = temp;
}


// From one set of buckets, calculate best fit line for delta_yi,
// to turn server times into client times. 
// If the server_is_smaller_ip, invert this mapping and make it
// turn client times into server times.
// In both cases, the remapped times will be in the time domain of the smaller ip address
void Fit(BucketStruct* cur_pair)  {
  const Bucket* buckets = cur_pair->buckets;
  Alignment* alignment = &cur_pair->t23_alignment;

//VERYTEMP
if (1 < cur_pair->entrynum) {DumpBuckets(stdout, buckets);}

  double n = 0.0;
  double x = 0.0;
  double y = 0.0;
  double xy = 0.0;
  double xx = 0.0;
  // Make base time the best fit in the first bucket
  ////int64 basetime = (buckets[0].tfrom / 60000000) * 60000000;
  int64 basetime = buckets[0].tfrom;
  // Later: try weighted sums or just double to 16 buckets
  for (int i = 0; i < kBucketNum; ++i) {
    if (buckets[i].slop == kEmptyBucket) {continue;}
    double xi = buckets[i].tfrom - basetime;
    double yi = (buckets[i].deltamin + buckets[i].deltamax) / 2.0;
    n += 1.0;
    x += xi;
    y += yi;
    xy += xi * yi;
    xx += xi * xi;
  }

  if (n != 0.0) {
    alignment->y0 = basetime;
    alignment->m = (n * xy - x * y) / (n * xx - x * x);
    alignment->b = (y - alignment->m * x) / n;
  } else {
    alignment->y0 = 0.0;
    alignment->m = 0.0;
    alignment->b = 0.0;
  }
  
  for (int i = 0; i < kBucketNum; ++i) {
    if (buckets[i].slop == kEmptyBucket) {continue;}
    double xi = buckets[i].tfrom - alignment->y0;
    ////double yi = (buckets[i].deltamin + buckets[i].deltamax) / 2.0;
    double delta_yi = alignment->m * xi + alignment->b;
    fprintf(stdout, "%6.1f ", delta_yi);
  }
  fprintf(stdout, "\n");

  // Cross-check by looking at the reverse mapping
  Alignment temp;
  InvertAlignment(alignment, &temp);

  for (int i = 0; i < kBucketNum; ++i) {
    if (buckets[i].slop == kEmptyBucket) {continue;}
    double xi = buckets[i].tfrom - temp.y0;
    ////double yi = (buckets[i].deltamin + buckets[i].deltamax) / 2.0;
    double delta_yi = temp.m * xi + temp.b;
    double xi_prime = xi + delta_yi;
    ////double delta_yi_prime = (temp.m * xi_prime + temp.b) / (1.0 - temp.m);
    double delta_yi_prime = temp.m * xi_prime + temp.b;
    fprintf(stdout, "%6.1f ", delta_yi_prime);
  }
  fprintf(stdout, "\n");
}

// Produce fname_minus_.xxx || s || .xxx
string FnameAppend(const char* fname, const char* s) {
  const char* period = strrchr(fname, '.');
  if (period == NULL) {period = fname + strlen(fname);}
  int len = period - fname;
  string retval = string(fname, len);
  retval += s;
  retval += period;
  return retval; 
}

inline uint32 Uint32Min(uint32 a, uint32 b) {return (a < b) ? a : b;}
inline uint32 Uint32Max(uint32 a, uint32 b) {return (a > b) ? a : b;}

void DumpLR(const char* label, BinaryLogRecord* lr) {
  fprintf(stdout, "%s %lld %lld %lld %lld\n", label, 
    lr->req_send_timestamp, lr->req_rcv_timestamp, 
    lr->resp_send_timestamp, lr->resp_rcv_timestamp);
}


// Handle extracting offsets from one log file at a time
void Pass1(const char* fname, BucketMap* bucketmap) {
  fprintf(stdout, "\nPass1: %s\n", fname);

  FILE* logfile = fopen(fname, "rb");
  if (logfile == NULL) {
    fprintf(stderr, "%s did not open\n", fname);
    return;
  }

  BinaryLogRecord lr;
  int64 basetime = 0;
  while(fread(&lr, sizeof(BinaryLogRecord), 1, logfile) != 0) {
////DumpLR("Pass1", &lr);

    // Skip unless at least t3 is there
    if (lr.resp_send_timestamp == 0) {continue;}

    // Estimated network transmission times
    int64 est_req_usec = RpcMsglglenToUsec(lr.lglen1);
    int64 est_resp_usec = RpcMsglglenToUsec(lr.lglen2);

    // Fill in any missing times (incomlete RPCs)
    // Missing t2 etc. must include estimated transmission time
    // Times in usec
    if (lr.req_rcv_timestamp == 0) {
      lr.req_rcv_timestamp = lr.req_send_timestamp + est_req_usec + kMissingTime;
    }
    if (lr.resp_send_timestamp == 0) {
      lr.resp_send_timestamp = lr.req_rcv_timestamp + kMissingTime;
    }
    if (lr.resp_rcv_timestamp == 0) {
      lr.resp_rcv_timestamp = lr.req_send_timestamp + 
        (lr.resp_send_timestamp - lr.req_rcv_timestamp) + 
        est_req_usec + kMissingTime + est_resp_usec + kMissingTime;
    }
////DumpLR("     ", &lr);


    int64 t1 = lr.req_send_timestamp;
    int64 t2 = lr.req_rcv_timestamp;
    int64 t3 = lr.resp_send_timestamp;
    int64 t4 = lr.resp_rcv_timestamp;

    // Only look at complete RPCs
    if (t4 == 0) {continue;}

    uint64 map_key = (static_cast<uint64>(lr.client_ip) << 32) | lr.server_ip;
    BucketMap::iterator it = bucketmap->find(map_key);
    if (it == bucketmap->end()) {
      //New machine pair
      BucketStruct temp;
      InitBucketStruct(&temp);
      (*bucketmap)[map_key] = temp;
      it = bucketmap->find(map_key);
//fprintf(stdout, "New pair: ip %08x %08x t1-t4 %lld %lld %lld %lld\n", 
//lr.client_ip, lr.server_ip, t1, t2, t3, t4);
    }
    BucketStruct* cur_pair = &it->second;

    int64 slop = (t4 - t1) - (t3 - t2) - est_req_usec - est_resp_usec;
    
// Do not want degenerate missing t3 and t4 to make tiny always-win slop

    
    // Arithmetic is wonky if zero or negative slop factored in
    if (slop < 2) slop = 2;	// Two usec minimum slop

    int cur_bucket = cur_pair->entrynum >> cur_pair->bucket_shift;
    if (cur_bucket >= kBucketNum) {
//fprintf(stdout, "  Halve buckets\n");
      // Compress into half as many buckets
      // DumpBuckets(stdout, cur_pair->buckets);
      for (int i = 0; i < (kBucketNum >> 1); ++i) {
        // Keep lower slop of a pair
        if (cur_pair->buckets[2 * i].slop <= cur_pair->buckets[2 * i + 1].slop) {
          cur_pair->buckets[i] = cur_pair->buckets[2 * i];
        } else {
          cur_pair->buckets[i] = cur_pair->buckets[2 * i + 1];
        }
      }
      ++cur_pair->bucket_shift;
      cur_bucket = cur_pair->entrynum >> cur_pair->bucket_shift;
      InitBuckets(cur_bucket, cur_pair->buckets);
    }

    if (slop < cur_pair->buckets[cur_bucket].slop) {
//fprintf(stdout, "  [%d] slop %lld @ %s\n", cur_bucket, slop, datetostr(t1));
      int64 deltamin = (t1 - t2) + est_req_usec;
      int64 deltamax = (t4 - t3) - est_resp_usec;
      if (deltamin >= deltamax) {
        // Oops, they crossed
        int64 mid = (deltamin + deltamax) / 2;
        deltamin = mid - 1;
        deltamax = mid + 1;
      }
      cur_pair->buckets[cur_bucket].slop = slop;
      cur_pair->buckets[cur_bucket].tfrom = (t2 + t3) / 2;
      cur_pair->buckets[cur_bucket].deltamin = deltamin;
      cur_pair->buckets[cur_bucket].deltamax = deltamax;
    }

    ++cur_pair->entrynum;
  }

  fclose(logfile);
}

// For each endpoint pair in the bucket map, calculate best fit line for delta_yi
void CalculateFits(BucketMap* bucketmap) {
  for (BucketMap::iterator it = bucketmap->begin(); it != bucketmap->end(); ++it) {
    uint64 map_key = it->first;
    uint32 client_ip = map_key >> 32;
    uint32 server_ip = map_key & 0x00000000ffffffffl;
    BucketStruct* cur_pair = &it->second;
    
    fprintf(stdout, "\nCalculateFits %08x <== %08x\n", client_ip, server_ip);

    Fit(cur_pair);

    DumpAlignments(stdout, cur_pair);
  }
}

// Find a client or server ip that is unmapped (to use it as the next base_ip)
uint32 FindUnmappedIp(const BucketMap* bucketmap, const IpToAlignment* iptoalignment) {
  for (BucketMap::const_iterator it = bucketmap->begin(); it != bucketmap->end(); ++it) {
    uint64 map_key = it->first;
    uint32 client_ip = map_key >> 32;
    uint32 server_ip = map_key & 0x00000000ffffffffl;
    const BucketStruct* cur_pair = &it->second;
    if (cur_pair->time_mapping_assigned) {continue;}
    // Return an unmapped ip. 

    if (iptoalignment->find(client_ip) == iptoalignment->end()) {return client_ip;}
    if (iptoalignment->find(server_ip) == iptoalignment->end()) {return server_ip;}
  }
  return 0;
}

bool IsMapped(const IpToAlignment* iptoalignment, uint32 ip) {
  return iptoalignment->find(ip) != iptoalignment->end();
}

// Transitively update alignments to map all times for all machines to 
// time on the lowest ip address encountered
// TODO: use map to pick off all machine pairs
//
// we have a collection of lo<=hi time mappings
// we want to change them so that they all map to the lowest
//  lowest <= A
//  lowest <= B
//  lowest <= C, etc.
// mark any that already map to lowest, then iteratively 
//  find Y s.t. Y does not map to lowest and Y maps to X and X maps to lowest
//  remap Y => X and X => lowest so that Y => lowest
// Iterate until all map to lowet or no change (some disconnected machines)
// either start over withlowest umapped, or comment and stop
//
void TransitiveAlignment(BucketMap* bucketmap) {
  // Create a time mapping for each ip address
  IpToAlignment iptoalignment;
  uint32 base_ip;
  while ((base_ip = FindUnmappedIp(bucketmap, &iptoalignment)) != 0) {
    // Add identity mapping base_ip => base_ip 
fprintf(stdout, "\nTransitiveAlignment, base is %08x\n", base_ip);
    Alignment temp;
    InitAlignment(&temp);
    iptoalignment[base_ip] = temp;
fprintf(stdout, "  iptoalignment[%08x] ", base_ip); DumpAlignment(stdout, &temp);
    bool changed;
    do {
      changed = false;
      for (BucketMap::iterator it = bucketmap->begin(); it != bucketmap->end(); ++it) {
        uint64 map_key = it->first;
        uint32 client_ip = map_key >> 32;
        uint32 server_ip = map_key & 0x00000000ffffffffl;
        BucketStruct* cur_pair = &it->second;
//if (1 < cur_pair->entrynum) {DumpBucketStruct(stdout, cur_pair);}
        if (cur_pair->time_mapping_assigned) {continue;}
        if (IsMapped(&iptoalignment, client_ip) && 
            !IsMapped(&iptoalignment, server_ip)) {
          // Have server==>client and client has known mapping to base
          // Merge server==>client and client==>base into server==>base
fprintf(stdout, "  Align %08x <== %08x to %08x\n", client_ip, server_ip, base_ip);
          const Alignment* tobase = &iptoalignment[client_ip];
          MergeAlignment(&cur_pair->t14_alignment, tobase, &cur_pair->t14_alignment);
          MergeAlignment(&cur_pair->t23_alignment, tobase, &cur_pair->t23_alignment);
          iptoalignment[client_ip] = cur_pair->t14_alignment;
fprintf(stdout, "  iptoalignment[%08x] ", client_ip); DumpAlignment(stdout, &cur_pair->t14_alignment);
          iptoalignment[server_ip] = cur_pair->t23_alignment;
fprintf(stdout, "  iptoalignment[%08x] ", server_ip); DumpAlignment(stdout, &cur_pair->t23_alignment);
          cur_pair->time_mapping_assigned = true;
          changed = true;
          DumpAlignments(stdout, cur_pair);
        } else if (IsMapped(&iptoalignment, server_ip) && 
                   !IsMapped(&iptoalignment, client_ip)) {
          // Have server==>client and server has known mapping to base
          // Invert server==>client to get client==>server 
          // Merge client==>server and server==>base into client==>base
fprintf(stdout, "  Align %08x ==> %08x to %08x\n", client_ip, server_ip, base_ip);
          const Alignment* tobase = &iptoalignment[server_ip];
          Alignment temp;
          InvertAlignment(&cur_pair->t23_alignment, &temp);
          InitAlignment(&cur_pair->t23_alignment);
          MergeAlignment(&temp, tobase, &cur_pair->t14_alignment);
          MergeAlignment(&cur_pair->t23_alignment, tobase, &cur_pair->t23_alignment);
          iptoalignment[client_ip] = cur_pair->t14_alignment;
fprintf(stdout, "  iptoalignment[%08x] ", client_ip); DumpAlignment(stdout, &cur_pair->t14_alignment);
          iptoalignment[server_ip] = cur_pair->t23_alignment;
fprintf(stdout, "  iptoalignment[%08x] ", server_ip); DumpAlignment(stdout, &cur_pair->t23_alignment);
          cur_pair->time_mapping_assigned = true;
          changed = true;
          DumpAlignments(stdout, cur_pair);
        }
      }
    } while (changed);
  }
}

// Read and rewrite log files, updating all times
void Pass2(const char* fname, const BucketMap* bucketmap) {
  fprintf(stdout, "\nPass2: %s\n", fname);

  FILE* logfile = fopen(fname, "rb");
  if (logfile == NULL) {
    fprintf(stderr, "%s did not open\n", fname);
    return;
  }

  string newfname = FnameAppend(fname, "_align");
  FILE* newlogfile = fopen(newfname.c_str(), "wb");
  if (newlogfile == NULL) {
    fprintf(stderr, "%s did not open\n", newfname.c_str());
    return;
  }

  BinaryLogRecord lr;
  int64 basetime = 0;
  while(fread(&lr, sizeof(BinaryLogRecord), 1, logfile) != 0) {
 
   // Align times: look at two ip values, find alignment, remap all four times
    int64 t1 = lr.req_send_timestamp;
    int64 t2 = lr.req_rcv_timestamp;
    int64 t3 = lr.resp_send_timestamp;
    int64 t4 = lr.resp_rcv_timestamp;

    uint64 map_key = (static_cast<uint64>(lr.client_ip) << 32) | lr.server_ip;
    BucketMap::const_iterator it = bucketmap->find(map_key);
    if (it == bucketmap->end()) {
      //fprintf(stdout, "ERROR: unknown ip pair %08x %08x\n", 
      //        lr.client_ip, lr.server_ip);
      fwrite(&lr, sizeof(BinaryLogRecord), 1, newlogfile); 
      continue;
    }
    const BucketStruct* cur_pair = &it->second;

    // Do the remapping
    const Alignment* t14 = &cur_pair->t14_alignment;
    const Alignment* t23 = &cur_pair->t23_alignment;

    int64 delta_t1, delta_t2, delta_t3, delta_t4;
    delta_t1 = t14->m * (t1 - t14->y0) + t14->b;
    delta_t2 = t23->m * (t2 - t23->y0) + t23->b;
    delta_t3 = t23->m * (t3 - t23->y0) + t23->b;
    delta_t4 = t14->m * (t4 - t14->y0) + t14->b;

    // Only update incoming nonzero values (zeros in incomplete transactions)
    if (lr.req_send_timestamp == 0) {delta_t1 = 0;}
    if (lr.req_rcv_timestamp == 0) {delta_t2 = 0;}
    if (lr.resp_send_timestamp == 0) {delta_t3 = 0;}
    if (lr.resp_rcv_timestamp == 0) {delta_t4 = 0;}

    // Enforce that nonzero times are non-decreasing
    if (lr.req_send_timestamp != 0) {
      lr.req_send_timestamp  = t1 + delta_t1;
    }
    if (lr.req_rcv_timestamp != 0) {
      lr.req_rcv_timestamp   = imax(t2 + delta_t2, lr.req_send_timestamp);
    }
    if (lr.resp_send_timestamp != 0) {
      lr.resp_send_timestamp = imax(t3 + delta_t3, lr.req_rcv_timestamp);
    }
    if (lr.resp_rcv_timestamp != 0) {
      lr.resp_rcv_timestamp  = imax(t4 + delta_t4, lr.resp_send_timestamp);
    }

/////fprintf(stdout, "%lld t1 += %lld,  t2 += %lld,  t3 += %lld,  t4 += %lld,  server_ip = %08x\n", 
////t1, delta_t1, delta_t2, delta_t3,  delta_t4, lr.server_ip);
 
////DumpLR("  new", &lr);

    fwrite(&lr, sizeof(BinaryLogRecord), 1, newlogfile);
  }

  fclose(logfile);
  fclose(newlogfile);
  fprintf(stderr, "  %s written\n", newfname.c_str()); 
}


int main(int argc, const char** argv) {
  bool dump_all = false;
  int next_arg = 1;
  if ((argc < 2) || (argv[1] == NULL)) {
    fprintf(stderr, "Usage: timealign <binary RPC log file name>+\n");
    return 0;
  }
  if ((argc > 2) && (strcmp(argv[2], "-all") == 0)) {
    dump_all = true;
    ++next_arg;
  }

  BucketMap bucketmap;

  for (int i = next_arg; i < argc; ++i) {
    if (strstr(argv[i], "_align.log") != 0) {continue;}
    Pass1(argv[i], &bucketmap);
  }

  CalculateFits(&bucketmap);

  TransitiveAlignment(&bucketmap);

  for (int i = next_arg; i < argc; ++i) {
    if (strstr(argv[i], "_align.log") != 0) {continue;}
    Pass2(argv[i], &bucketmap);
  }

  return 0;
}

