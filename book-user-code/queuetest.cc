// Little program to exercise queues
// Copyright 2021 Richard L. Sites
//
// This program cranks up several tasks that service "work" entries on queues,
// each task looping CPU-bound for a specified number of microseconds and then
// passing the work task on to a subsequent queue. The last queue is always 
// number zero, which finishes the work entry and deletes it.
// The main program produces N work entries and then exits. Each piece of work
// is a pseudo-RPC. It is logged at beginning and end, using the dclab_log 
// framework that is  used for other RPCs.
//
// Multiple work entries are active at once and they may interfere with each 
// other. Some entries will wait in queues for previously-queued work. The
// net effect is that some work entries will take much longer than might be 
// expected. 
//
// Using KUtrace with this program will show the dynamics that lead to such
// long transaction delays.
//
// Work entries are produced by the main program at varying intervals, some of
// which are quite short. The entries specify varying sequences of queues to
// sequence through and varying amounts of "work" for each queue task. The
// variations come in two forms, uniform pseudo-random intervals, and skewed 
// ones. The sequences also come in uniform and skewed forms, with the latter
// putting more work into some queues and less work into others.
//
// Command-line parameters:
//   -rate <num>	generate approximately num transactions per second
//   -n <num>		generate num transactions and then stop
//   -skew		use skewed intervals and queues (default is uniform)
//   -s			show acquire/release for spinlocks (debug aid)
//   -v			verbose
//
// Outputs:
//   dclab transaction log file written to constructed file name of form
//   queuetest_20210126_145625_dclab-2_2614.log
//
//   Number of transactions, number dropped as too busy
//
// compile with g++ -O2 -pthread queuetest.cc dclab_log.cc dclab_rpc.cc kutrace_lib.cc -o queuetest

#include <string>

#include <linux/futex.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>	// for SYS_xxx definitions
#include <time.h>		// for nanosleep
#include <unistd.h>		// for syscall

#include "basetypes.h"
#include "dclab_log.h"
#include "dclab_rpc.h"
#include "kutrace_lib.h"
#include "polynomial.h"
#include "timecounters.h"

using std::string;

static const int kMaxTransInFlight = 40;

static const uint32 kMaxShortQUsec = 1000;  // Uniform/skew average  0.5/0.75 * this
static const uint32 kMaxLongQUsec =  4000;  // Uniform/skew average  0.5/0.75 * this

static const int kIterations = 140;	// NOTE: 140 is ~1 usec loop on Intel i3
					// Adjust as needed


// Queue tasks use futex to wait on empty queues

// Queue zero task launches and also terminates transactions. It keeps a count
// of transactions in flight and rejects new transactions with "too busy"
// status code if the number in flight is above a specified limit. If the rate
// of new transactions exceeds the rate of finishing transactions, the number
// in flight will saturate near this limit. This models real datacenter
// overload behavior that would result in HTTP 503 "Service Unavailable" error 
// response status codes.

//
// Work item. It includes next pointer for queueing
//
typedef struct {
  uint32 queue_num;	// Queue number to be on
  uint32 usec_busy;	// How much work to do
} OneWork;

typedef struct WorkT {
  WorkT* next;
  int trans_num;	// To trackhow long each takes
  OneWork onework[4];	// Up to four steps of work to do
  BinaryLogRecord log;	// Mostly to log start/stop times as pseudo-RPC
} Work;


//
// Simple queue of work to do
// Only manipulated after acquiring lock (i.e. lock is set)
//
typedef struct {
  Work* head;
  Work* tail;
  int count;	// Number of items on this queue
  char lock;
} Queue;

typedef struct {
  Queue* queue;		// Array of all queues (so work can be passed around)
  FILE* logfile;	// Output log file (PrimaryTask writes once per transaction)
  uint32 i;		// Which queue we are processing
} PerThreadData;



//
// Work patterns
//
// Queue[0] is start/stop
// Queues [1..3] are shorter amount of work
// Queues [4..7] are longer amount of work

// Average M is 1000+4000 = 5000us
// At average time = M * 0.5, each transaction is about 2.5 ms, or 400/sec per CPU
// This gives about 1600 trans/sec for 4 CPUs. Any rate faster than this will
// get behind and start dropping work
static const OneWork kUniformWorkPattern[16][4] = {
  {{1, kMaxShortQUsec}, {4, kMaxLongQUsec}, {0, 0}, {0, 0}},
  {{2, kMaxShortQUsec}, {5, kMaxLongQUsec}, {0, 0}, {0, 0}},
  {{3, kMaxShortQUsec}, {4, kMaxLongQUsec}, {0, 0}, {0, 0}},
  {{1, kMaxShortQUsec}, {5, kMaxLongQUsec}, {0, 0}, {0, 0}},
  {{2, kMaxShortQUsec}, {4, kMaxLongQUsec}, {0, 0}, {0, 0}},
  {{3, kMaxShortQUsec}, {5, kMaxLongQUsec}, {0, 0}, {0, 0}},
  {{1, kMaxShortQUsec}, {4, kMaxLongQUsec}, {0, 0}, {0, 0}},
  {{2, kMaxShortQUsec}, {5, kMaxLongQUsec}, {0, 0}, {0, 0}},

  {{3, kMaxShortQUsec}, {4, kMaxLongQUsec}, {0, 0}, {0, 0}},
  {{1, kMaxShortQUsec}, {5, kMaxLongQUsec}, {0, 0}, {0, 0}},
  {{2, kMaxShortQUsec}, {4, kMaxLongQUsec}, {0, 0}, {0, 0}},
  {{3, kMaxShortQUsec}, {5, kMaxLongQUsec}, {0, 0}, {0, 0}},
  {{1, kMaxShortQUsec}, {4, kMaxLongQUsec}, {0, 0}, {0, 0}},
  {{2, kMaxShortQUsec}, {5, kMaxLongQUsec}, {0, 0}, {0, 0}},
  {{3, kMaxShortQUsec}, {4, kMaxLongQUsec}, {0, 0}, {0, 0}},
  {{1, kMaxShortQUsec}, {5, kMaxLongQUsec}, {0, 0}, {0, 0}},
};

// Average M is 1000 + 4000 + 4000/4 = 6000us
// At average time = M * 0.75, each transaction is about 4.5 ms, or 222/sec per CPU
// This gives about 888 trans/sec for 4 CPUs. Any rate faster than this will
// get behind and start dropping work
static const OneWork kSkewedWorkPattern[16][4] = {
  {{1, kMaxShortQUsec}, {4, kMaxLongQUsec}, {0, 0}, {0, 0}},
  {{2, kMaxShortQUsec}, {5, kMaxLongQUsec}, {0, 0}, {0, 0}},
  {{1, kMaxShortQUsec}, {6, kMaxLongQUsec}, {0, 0}, {0, 0}},
  {{3, kMaxShortQUsec}, {4, kMaxLongQUsec}, {5, kMaxLongQUsec}, {0, 0}},
  {{1, kMaxShortQUsec}, {4, kMaxLongQUsec}, {0, 0}, {0, 0}},
  {{2, kMaxShortQUsec}, {5, kMaxLongQUsec}, {0, 0}, {0, 0}},
  {{1, kMaxShortQUsec}, {6, kMaxLongQUsec}, {0, 0}, {0, 0}},
  {{3, kMaxShortQUsec}, {4, kMaxLongQUsec}, {5, kMaxLongQUsec}, {0, 0}},

  {{1, kMaxShortQUsec}, {4, kMaxLongQUsec}, {0, 0}, {0, 0}},
  {{2, kMaxShortQUsec}, {5, kMaxLongQUsec}, {0, 0}, {0, 0}},
  {{1, kMaxShortQUsec}, {6, kMaxLongQUsec}, {0, 0}, {0, 0}},
  {{3, kMaxShortQUsec}, {4, kMaxLongQUsec}, {5, kMaxLongQUsec}, {0, 0}},
  {{1, kMaxShortQUsec}, {4, kMaxLongQUsec}, {0, 0}, {0, 0}},
  {{2, kMaxShortQUsec}, {5, kMaxLongQUsec}, {0, 0}, {0, 0}},
  {{1, kMaxShortQUsec}, {6, kMaxLongQUsec}, {0, 0}, {0, 0}},
  {{3, kMaxShortQUsec}, {4, kMaxLongQUsec}, {5, kMaxLongQUsec}, {0, 0}},
};

// Globals
static bool nevertrue;	// Set false in main program, but compiler doesn't know the value

static bool trace_spinlocks = false;
static bool verbose = false;

static volatile int pending_count = 0;
static volatile int dropped_count = 0;
static int64* delay_times;		// In usec
static int64* transaction_times;	// In usec

static char serial_lock = 0;	// For debugging, uncomment the uses of this

inline uint64 uint32max(uint32 a, uint32 b) {return (a > b) ? a : b;}
inline uint64 uint32min(uint32 a, uint32 b) {return (a < b) ? a : b;}

inline uint32 Rrange8(uint32 rand) {return rand & 0xFF;}
inline uint32 Rscale4(uint32 rand) {return (rand >> 8) & 0xF;}
inline uint32 Rdelay8(uint32 rand) {return (rand >> 24) & 0xFF;}

void  UpdateRand(uint32* rand) {
  *rand = POLYSHIFT32(*rand);
  *rand = POLYSHIFT32(*rand);
  *rand = POLYSHIFT32(*rand);
}

// Sleep for n microseconds
void usecsleep(uint32 usec) {
  struct timespec ts;
  ts.tv_sec = usec / 1000000;
  ts.tv_nsec = (usec % 1000000) * 1000;
  nanosleep(&ts, NULL);
}

// Returns average waiting usec to get the approximate given rate of transactions/sec
// Uniform rate is M * 0.5, where M is the maximum delay
// Skewed rate is M * 0.75
// So for 1000/sec, uniform is 2000000 usec / 1000 gives M = 2000 usec
// So for 1000/sec, skewed  is 4000000 usec / 3000 gives M = 1333 usec
uint32 RateToMax(uint32 rate, bool skew) { 
  if (!skew) {return 2000000 / rate;}
  return             4000000 / (rate * 3);
}

// For skewed distribution, use range [0..max*8)
// Input value uniform is already M * 0.5
uint32 Skewed(uint32 rand, uint32 uniform) {
  uint32 scale = Rscale4(rand);
  if (scale & 1) {return uniform >> 1;}	// xxx1 8*0.5 = 4
  if (scale & 2) {return uniform;}	// xx10 4*1 = 4
  if (scale & 4) {return uniform << 1;}	// x100 2*2 = 4
  if (scale & 8) {return uniform << 2;}	// 1000 1*4 = 4
  return                 uniform << 3;	// 0000 1*8 = 8
					//      avg = 24/16 = 1.5*uniform = 0.75 * M
}

// For uniform distribution, use range [0..max)
uint32 GetWorkRand(uint32 rand, int max, bool skew) {
  // max * [0..255]/256
  uint32 uniform = (max * Rrange8(rand)) >> 8;
  if (!skew) {return uniform;}			// M * 0.5 avg
  return Skewed(rand, uniform);			// M * 0.75
}

// Result in microseconds, [0..max) for uniform
uint32 GetDelayRand(uint32 rand, int max, bool skew) {
  // max * [0..255]/256
  uint32 uniform = (max * Rdelay8(rand)) >> 8;
  if (!skew) {return uniform;}			// M * 0.5 avg
  return Skewed(rand, uniform);			// M * 0.75
}


//
// Plain spinlock. It just covers some few-instruction sequences so almost never blocks
//
// The constructor acquires the spinlock and the destructor releases it.
// Thus, just declaring one of these in a block makes the block run *only* when 
// holding the lock and then reliably release it at block exit 
class PlainSpinLock {
public:
  PlainSpinLock(volatile char* lock);
  ~PlainSpinLock();
  volatile char* lock_;
};

PlainSpinLock::PlainSpinLock(volatile char* lock) {
  lock_ = lock;
  bool already_set;
  if (trace_spinlocks) kutrace::mark_b("a");
  do {
    while (*lock_ != 0) {}  // Spin without writing while someone else holds the lock
    // Try to get the lock
    already_set = __atomic_test_and_set(lock_, __ATOMIC_ACQUIRE);
  } while (already_set);
  if (trace_spinlocks) kutrace::mark_b("/");
}

PlainSpinLock::~PlainSpinLock() {
  if (trace_spinlocks) kutrace::mark_b("r");
  __atomic_clear(lock_, __ATOMIC_RELEASE);
}

//
// Simple queue of work to do
//

void InitQueue(Queue* queue) {
  memset(queue, 0, sizeof(Queue));
}

void DumpQueues(FILE* f, const char* label, Queue* queue) {
  for (int i = 0; i < 8; ++i) {
    PlainSpinLock spinlock(&queue[i].lock);
    fprintf(f, "%s dumpQueues[%d] ", label, i);
    const Work* p = queue[i].head;
    while (p != NULL) {
      fprintf(f, "%08x ", p->log.rpcid);
      p = p->next;
    }
    fprintf(f, "\n");
  }
}


void EnqueueBad(Work* item, Queue* queue, int queue_num) {
  PlainSpinLock spinlock(&queue->lock);
////fprintf(stderr, "Enqueue %08x on %d\n", item->log.rpcid, queue_num);

  item->next = NULL;
  if (queue->head == NULL) {
    queue->head = item;
  } else {
    queue->tail->next = item;
  }
  queue->tail = item;
  ++queue->count;
  kutrace::addevent(KUTRACE_ENQUEUE, queue_num);
  syscall(SYS_futex, &queue->count, FUTEX_WAKE, 0, NULL, NULL, 0);
  // BUG
  // We are still holding the spinlock when FUTEX_WAKE returns. Awakening process
  // may spin a little. Or a WHOLE LOT if we get context switched out inside futex...
}

void EnqueueFixed(Work* item, Queue* queue, int queue_num) {
  do {
    PlainSpinLock spinlock(&queue->lock);
////fprintf(stderr, "Enqueue %08x on %d\n", item->log.rpcid, queue_num);

    item->next = NULL;
    if (queue->head == NULL) {
      queue->head = item;
    } else {
      queue->tail->next = item;
    }
    queue->tail = item;
    ++queue->count;
  } while(false);
  // Spinlock is now released. 
  kutrace::addevent(KUTRACE_ENQUEUE, queue_num);
  syscall(SYS_futex, &queue->count, FUTEX_WAKE, 0, NULL, NULL, 0);
}

#ifdef FIXED
inline void Enqueue(Work* item, Queue* queue, int queue_num) {EnqueueFixed(item, queue, queue_num);}
#else
inline void Enqueue(Work* item, Queue* queue, int queue_num) {EnqueueBad(item, queue, queue_num);} 
#endif

Work* Dequeue(Queue* queue, int queue_num) {
  PlainSpinLock spinlock(&queue->lock);
  kutrace::addevent(KUTRACE_DEQUEUE, queue_num);

  Work* item = queue->head;
  queue->head = item->next;	// Note: When this goes NULL, tail is garbage
  --queue->count;
////fprintf(stderr, "Dequeue %08x from %d\n", item->log.rpcid, queue_num);
  return item;
}

uint32 GetRpcid(uint32* rand) {
  uint32 retval = *rand;
  UpdateRand(rand);
  return retval;
}

void InitWork(Work* work) {
  memset(work, 0, sizeof(Work));
}

void DumpWork(FILE* f, const Work* work, bool brief) {
  if (brief) {
    fprintf(f, "%5d: ", work->log.rpcid);
    for (int i = 0; i < 4; ++i) {
      fprintf(f, "%u %u   ", work->onework[i].queue_num, work->onework[i].usec_busy);
    }
    fprintf(f, "\n");
    return;
  }

  fprintf(f, "DumpWork\n");
  for (int i = 0; i < 4; ++i) {
    fprintf(f, "%u %u   ", work->onework[i].queue_num, work->onework[i].usec_busy);
  }
  fprintf(f, "\n");
  PrintLogRecord(f, &work->log);
}

Work* CreateWork(int trans_num, uint32* rand, bool skew) {
  Work* work = new Work;
  InitWork(work);
  work->trans_num = trans_num;
  // Fill in the logrecord fields
  work->log.rpcid = rpcid32_to_rpcid16(GetRpcid(rand));
  work->log.req_send_timestamp = GetUsec();
  work->log.lglen1 = TenLg(sizeof(Work));
  work->log.lglen2 = work->log.lglen1;
  strcpy(work->log.method, "Work");
  work->log.datalength = sizeof(Work);

  // Fill in the actual work specification from selected pattern
  uint32 select = Rscale4(*rand); 	// Which of 16 patterns to use
  UpdateRand(rand);

  const OneWork* pattern = (skew ? kSkewedWorkPattern : kUniformWorkPattern)[select];
  for (int i = 0; i < 4; ++i) {
    work->onework[i].queue_num = pattern[i].queue_num;
    work->onework[i].usec_busy = GetDelayRand(*rand, pattern[i].usec_busy, skew);
    UpdateRand(rand);
  }

if (verbose) DumpWork(stderr, work, true);
  // DumpWork(stderr, work, false);
  return work;
}

void DeleteWork(Work* work) {
  delete work;
}

// Constructs N work entries and send them to primary queue
void GenerateLoop(int n, uint32 rate, bool skew, Queue* primaryqueue) {
  uint32 rand = POLYINIT32;
  uint32 max_delay_usec = RateToMax(rate, skew); 

  for (int i = 0; i < n; ++i) {
    kutrace::mark_d(pending_count);
    Work* work = CreateWork(i, &rand, skew);

    kutrace::addname(KUTRACE_METHODNAME, work->log.rpcid, work->log.method);
    kutrace::addevent(KUTRACE_RPCIDREQ, work->log.rpcid);
    Enqueue(work, primaryqueue, 0);
    kutrace::addevent(KUTRACE_RPCIDREQ, 0);

    // Wait xx microseconds
    uint32 wait_usec = GetDelayRand(rand, max_delay_usec, skew);
    UpdateRand(&rand);
    delay_times[i] = wait_usec;
    usecsleep(wait_usec);
  }

  // Wait for transactions to finish, pending_count == 0, before returning
  // MINOR BUG: this can stop early if the first RPC has not yet been pulled off by PrimaryTask, 
  // which makes pending_count non-zero... however, we wait several usec before getting here.
  // But if we increment pending-count here, we can queue more than 50 items and then
  //  PrimaryTask will delete the early ones instead of the late ones.
  //  On the other hand, a real client would be getting response messages so no problem...
  kutrace::mark_a("finish");
  while (pending_count != 0) {}

  kutrace::mark_a("/");
}


// PrimaryTask launches and terminates work, logging each begin and end.
// Every work entry comes here twice, at beginning and end.
// A special "stop" work entry causes PrimaryTask to wait until all previous 
// work entries have finished and then it terminates.
void* PrimaryTask(void* arg) {
  PerThreadData* perthreaddata = reinterpret_cast<PerThreadData*>(arg);
  int ii = perthreaddata->i;
  Queue* queue = perthreaddata->queue;
  Queue* myqueue = &queue[ii];
  FILE* logfile = perthreaddata->logfile;
  fprintf(stderr, "  PrimaryTask starting, queue %d\n", ii);

  // Loop:
  // Remove queue entry (waits if empty)
  // If new request
  //   Set req_rcv_timestamp
  //   if pending_count at kMaxTransInFlight, 
  //     increment dropped_count
  //     set status = "too busy"
  //     go straight to completion, dropping the request
  //   Increment pending_count
  //   Enqueue to first work queue
  // If completed request
  //   Decrement pending_count
  //   Set resp_send_timestamp, resp_rcv_timestamp
  //   log the request

  do {
    while(myqueue->count == 0) {
      // Wait for some work
      syscall(SYS_futex, &myqueue->count, FUTEX_WAIT, 0, NULL, NULL, 0);
    }
    // We have a real work item now
    // No locks are needed around pending_count because we are the only thread that changes it.
    Work* item = Dequeue(myqueue, ii);
    kutrace::addevent(KUTRACE_RPCIDREQ, item->log.rpcid);
////fprintf(stderr, "PrimaryTask[%d], pending %d\n", ii, pending_count);
////DumpWork(stderr, item, true);

    uint32 next_q = item->onework[0].queue_num;
    if (next_q != 0) {
      // There is work to do on initial queue N, but we might be too busy
      item->log.req_rcv_timestamp = GetUsec();
      ++pending_count;
      if (pending_count <= kMaxTransInFlight) {
        // Not too busy. Move the item to another queue
        Enqueue(item, &queue[next_q], next_q);
        kutrace::addevent(KUTRACE_RPCIDREQ,0);
        continue;
      } else {
        ++dropped_count;
        item->log.status = TooBusyStatus;
        kutrace::mark_c("drop");
      }
    }

    // All done with this item or too busy. Finish up, log, and free
    item->log.type = RespRcvType;
    item->log.resp_send_timestamp = GetUsec();
    item->log.resp_rcv_timestamp = item->log.resp_send_timestamp + 0.000001;
    fwrite(&item->log, 1, sizeof(BinaryLogRecord), logfile);
    transaction_times[item->trans_num] = item->log.resp_rcv_timestamp - item->log.req_send_timestamp;
    --pending_count;
    DeleteWork(item);
    kutrace::addevent(KUTRACE_RPCIDREQ,0);
  } while (true);
}


double fdiv_wait_usec(uint32 usec) {
  double divd = 123456789.0;
  for (int i = 0; i < (usec * kIterations); ++i) {
    divd /= 1.0000001;
    divd /= 0.9999999;
  }
  if (nevertrue) {fprintf(stderr, "%f\n", divd);}	// Make live
  return divd;	// Make live (but only if caller uses it)
}


// Worker task loops doing specified work on a given queue
void* WorkerTask(void* arg) {
  PerThreadData* perthreaddata = reinterpret_cast<PerThreadData*>(arg);
  int ii = perthreaddata->i;
  Queue* queue = perthreaddata->queue;
  Queue* myqueue = &queue[ii];
  fprintf(stderr, "  WorkerTask starting, queue %d\n", ii);

  // Loop:
  // Remove queue entry (waits if empty)
  // KUtrace the rpcid
  // Do "work" for N microseconds
  // pop work off the list
  // KUtrace back to idle (rpcid 0)
  // Enqueue on next queue

  do {
    while(myqueue->count == 0) {
      // Wait for some work
      syscall(SYS_futex, &myqueue->count, FUTEX_WAIT, 0, NULL, NULL, 0);
    }
    // We have a real work item now; primary inserted the method name
    Work* item = Dequeue(myqueue, ii);
////fprintf(stderr, "WorkerTask[%d]\n", ii);
////DumpWork(stderr, item, true);
    kutrace::addevent(KUTRACE_RPCIDREQ, item->log.rpcid);
    uint32 for_q = item->onework[0].queue_num;
    if (for_q != ii) {
      fprintf(stderr, "BUG. Work for queue %d but on queue %d\n", for_q, ii);
    }
    uint32 usec = item->onework[0].usec_busy;

    // Fake "work" for N microseconds
    double unused = fdiv_wait_usec(usec);

    // Pop the list
    item->onework[0] = item->onework[1];
    item->onework[1] = item->onework[2];
    item->onework[2] = item->onework[3];
    item->onework[3].queue_num = 0;
    item->onework[3].usec_busy = 0;

    // On to the next queue; queue[0] will terminate item
    uint32 next_q = item->onework[0].queue_num;
    Enqueue(item, &queue[next_q], next_q);
    kutrace::addevent(KUTRACE_RPCIDREQ, 0);
  } while (true);
}

// Argument queue points to the array of queues
typedef void* (*QueueTask)(void*);
void CreateThreadForQueue(int i, Queue* queue, FILE* logfile, QueueTask qt) {
    // Allocate a per-thread data structure and fill it in
    PerThreadData* perthreaddata = new PerThreadData;
    perthreaddata->i = i;
    perthreaddata->queue = queue;
    perthreaddata->logfile = logfile;
    pthread_t thread; 
    int iret = pthread_create( &thread, NULL, qt, (void*) perthreaddata);
    if (iret != 0) {Error("pthread_create()", iret);}
}

int main (int argc, const char** argv) {
  // Self-tracing if KUtrace module is loaded
  kutrace::goipc(argv[0]);

  Queue queue[8];	// queue[0] feeds the primary task; [7] is unused
  for (int i = 0; i < 8; ++i) {
    InitQueue(&queue[i]);
  }

  // To make things live in fdiv_wait_usec
  nevertrue = (GetUsec() == 0);		// Compiler doen't know this is false 

  // Parse command line
  uint32 n = 100;	// Default
  uint32 rate = 1000;	// Default
  bool skew = false;	// Default
  for (int i = 1; i < argc; ++i) {
    if ((strcmp(argv[i], "-n") == 0) && (i < argc-1)) {
      n = atoi(argv[++i]);
    }
    if ((strcmp(argv[i], "-rate") == 0) && (i < argc-1)) {
      rate = atoi(argv[++i]);
    }
    if (strcmp(argv[i], "-skew") == 0) {skew = true;}
    if (strcmp(argv[i], "-s") == 0) {trace_spinlocks = true;}
    if (strcmp(argv[i], "-v") == 0) {verbose = true;}
  } 
fprintf(stderr, "n/rate/skew %u %u %u\n", n, rate, skew);

  // Set up globals
  pending_count = 0;
  dropped_count = 0;

  delay_times = new int64[n];
  transaction_times = new int64[n];

#if 0
// Calibrate fdiv loop
fprintf(stderr, "%016lld usec before nominal one second loop\n", GetUsec());
double unused = fdiv_wait_usec(1000000);
fprintf(stderr, "%016lld usec after\n", GetUsec());
#endif

  // Open log file
  const char* fname = MakeLogFileName(argv[0]);
  FILE* logfile = OpenLogFileOrDie(fname);

  // Spawn eight queue tasks
  // PrimaryTask(&queue[0])
  // for i=1..6 WorkerTask(&queue[i])
  for (int i = 0; i < 7; ++i) {
    fprintf(stderr, "queuetest: launching a thread to process queue %d\n", i);
    char temp[64];
    snprintf(temp, 64, "queue~%d", i);
    kutrace::addname(KUTRACE_QUEUE_NAME, i, temp);
    CreateThreadForQueue(i, queue, logfile, (i == 0) ? PrimaryTask : WorkerTask);
  }

  // Produce n transactions 
  // Wait for pending_count to drop to zero
  GenerateLoop(n, rate, skew, &queue[0]);

  fprintf(stderr, "\n%d transactions, %d dropped\n", n, dropped_count);

  // Close log file
  fclose(logfile);

  // Calculate a few statistics
  int64 sum_delay = 0;
  int64 sum_trans = 0;
  for (int i = 0; i < n; ++i) {
    sum_delay += delay_times[i];
    sum_trans += transaction_times[i];
  }
  fprintf(stdout, "\n");

  fprintf(stdout, "Delays (usec), total = %lld, average = %lld\n", sum_delay, sum_delay / n);
  if (verbose) {
    for (int i = 0; i < n; ++i) {
      fprintf(stdout, "%lld ", delay_times[i]);
      if ((i % 20) == 19) {fprintf(stdout, "\n");}
    }
    fprintf(stdout, "\n");
  }

  fprintf(stdout, "Transactions (usec), total = %lld, average = %lld\n", sum_trans, sum_trans / n);
  if (verbose) {
    for (int i = 0; i < n; ++i) {
      fprintf(stdout, "%lld ", transaction_times[i]);
      if ((i % 20) == 19) {fprintf(stdout, "\n");}
    }
    fprintf(stdout, "\n");
  }
  
  delete[] delay_times;
  delete[] transaction_times;
  // Get log file name near the end of theprintout
  fprintf(stdout, "  %s written\n", fname);

  // Self-tracing
  char namebuf[256];
  kutrace::stop(kutrace::MakeTraceFileName("qt", namebuf));

  // Exit, deleting the spawned tasks
  return 0;
}


