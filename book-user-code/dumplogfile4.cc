// dumplogfile4.cc cloned from dumplogfile.cc 2018.04.16
// Little program to dump a binary log file
// Copyright 2021 Richard L. Sites
//
// compile with g++ -O2 dumplogfile4.cc dclab_log.cc -o dumplogfile4
//
// expect filename(s) to be of form
//   client4_20180416_151126_dclab-1_3162.log
//
// Hex dump a log file:
//   od -Ax -tx4z -w32 foo.log
//

#include <stdio.h>
#include <string.h>

#include "dclab_log.h"

// Assumed Ethernet speed in gigabits per second
static const int64 kGbs = 1;

// Assumed RPC message overhead, in addition to pure data
static const int64 kMsgOverheadBytes = 100;

// Assumed time for missing transmission or server time, in usec
static const int kMissingTime = 2;

static char gTempBuffer[24];

// 2**0.0 through 2** 0.9
static const double kPowerTwoTenths[10] = {
  1.0000, 1.0718, 1.1487, 1.2311, 1.3195, 
  1.4142, 1.5157, 1.6245, 1.7411, 1.8661
};

int64 imax(int64 a, int64 b) {return (a >= b) ? a : b;}

// return 2 * (x/10)
int64 ExpTenths(uint8 x) {
  int64 powertwo = x / 10; 
  int64 fraction = x % 10;
  int64 retval = 1l << powertwo;
  retval *= kPowerTwoTenths[fraction];
  return retval;
}


// Return sec to transmit x bytes at y Gb/s, where 1 Gb/s = 125000000 B/sec
// but we assume we only get about 90% of this for real data, so 110 B/usec
int64 BytesToUsec(int64 x) {
  int64 retval = x * kGbs / 110;
  return retval;
}

int64 RpcMsglglenToUsec(uint8 lglen) {
  return BytesToUsec(ExpTenths(lglen) + kMsgOverheadBytes);
}


// Turn seconds since the epoch into yyyy-mm-dd_hh:mm:ss
// Not valid after January 19, 2038
const char* FormatSecondsDateTimeLong(int64 sec) {
  // if (sec == 0) {return "unknown";}  // Longer spelling: caller expecting date
  time_t tt = sec;
  struct tm* t = localtime(&tt);
  sprintf(gTempBuffer, "%04d-%02d-%02d_%02d:%02d:%02d", 
         t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, 
         t->tm_hour, t->tm_min, t->tm_sec);
  return gTempBuffer;
}

void PrintJsonHeader(FILE* f, int64 basetime, const char* title) {
  // Convert usec to sec and format date_time
  const char* base_char = FormatSecondsDateTimeLong(basetime / 1000000);
  // Leading spaces force header lines to sort to front
  fprintf(f, "  {\n");
  fprintf(f, " \"Comment\" : \"V4 flat RPCs\",\n");
  fprintf(f, " \"axisLabelX\" : \"Time (sec)\",\n");
  fprintf(f, " \"axisLabelY\" : \"RPC Number\",\n");
  fprintf(f, " \"deltaT23\" : 0,\n");
  fprintf(f, " \"flags\" : 0,\n");
  fprintf(f, " \"gbs\" : 1,\n");
  fprintf(f, " \"shortMulX\" : 1,\n");
  fprintf(f, " \"shortUnitsX\" : \"s\",\n");
  fprintf(f, " \"thousandsX\" : 1000,\n");
  fprintf(f, " \"title\" : \"%s\",\n", title);
  fprintf(f, " \"tracebase\" : \"%s\",\n", base_char);
  fprintf(f, " \"version\" : 4,\n");
  fprintf(f, "\"events\" : [\n");
}

void PrintJsonFooter(FILE* f) {
  fprintf(f, "[999.0, 0.0, 0.0, 0.0, \"\", \"\", 0.0, 0.0, 0, 0, \"\", \"\", \"\", 0, \"\"]\n");
  fprintf(f, "]}\n");
}

void usage() {
  fprintf(stderr, "Usage: dumplogfile4 [-all] [-req] \"title\" <binary file name(s)>\n");
  fprintf(stderr, "       By default, only complete (client type RespRcv) transactions are dumped.\n");
  fprintf(stderr, "       Use -all to see incomplete transactions (server side are all incomlete).\n");
}

static const int kMaxFileNames = 100;

int main(int argc, const char** argv) {
  bool dump_raw = false;
  bool dump_all = false;
  bool dump_req = false;
  int next_fname = 0;
  const char* fname[kMaxFileNames];
  const char* title = NULL;

  // Pick up arguments
  for (int i = 1; i < argc; ++i) {
    if (argv[i][0] != '-') {
      if (title == NULL) {
        title = argv[i];
      } else {
        fname[next_fname++] = argv[i];
        if (next_fname >= kMaxFileNames) {
          fprintf(stderr, "More than %d file names.\n", kMaxFileNames);
          return 0;
        }
      }
    } else if (strcmp(argv[i], "-raw") == 0) {
      dump_raw = true;
    } else if (strcmp(argv[i], "-all") == 0) {
      dump_all = true;
    } else if (strcmp(argv[i], "-req") == 0) {
      dump_req = true;
    } else {
      usage();
      return 0;
    }
  }

  if (next_fname == 0) {
    usage();
    return 0;
  }

  if (title == NULL) {title = "Placeholder title";}

  FILE* logfile;
  BinaryLogRecord lr;
  int64 basetime = 0;	// In usec
  // Process log files in order presented
  for (int i = 0; i < next_fname; ++i) {
    logfile = fopen(fname[i], "rb");
    if (logfile == NULL) {
      fprintf(stderr, "%s did not open\n", fname[i]);
      return 0;
    }

    // Always dump complete transactions, from client-side logs RespRcvType
    // If -req, dump completed server-side requests RespSendType
    // If -all, dump all log records
    while(fread(&lr, sizeof(BinaryLogRecord), 1, logfile) != 0) {
      bool dumpme = false;
      if (dump_all) {dumpme = true;}
      if (dump_req && lr.type == RespSendType) {dumpme = true;}
      if (lr.type == RespRcvType) {dumpme = true;}
      if (!dumpme) {continue;}

      // Pick off base time at first RPC
      if ((basetime == 0) && (lr.req_send_timestamp != 0)) {
        // Round down usec time to multiple of one minute
        basetime = (lr.req_send_timestamp / 60000000) * 60000000;
        PrintJsonHeader(stdout, basetime, title);
      }

      // Estimated network transmission times
      int64 est_req_usec = RpcMsglglenToUsec(lr.lglen1);
      int64 est_resp_usec = RpcMsglglenToUsec(lr.lglen2);

      if (!dump_raw) {
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

        // Enforce that nonzero times are non-decreasing
        if (lr.req_rcv_timestamp != 0) {
          lr.req_rcv_timestamp   = imax(lr.req_rcv_timestamp, lr.req_send_timestamp);
        }
        if (lr.resp_send_timestamp != 0) {
          lr.resp_send_timestamp = imax(lr.resp_send_timestamp, lr.req_rcv_timestamp);
        }
        if (lr.resp_rcv_timestamp != 0) {
          lr.resp_rcv_timestamp  = imax(lr.resp_rcv_timestamp, lr.resp_send_timestamp);
        }
      }

      PrintLogRecordAsJson(stdout, &lr, basetime);
    }
    fclose(logfile);
  }
  PrintJsonFooter(stdout);

  return 0;
}

