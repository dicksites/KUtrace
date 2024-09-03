// Little program to turn sorted Ascii event listings into timespans
// covering 100% of the time on each CPU core
// The main work is tracking returns and dealing with missing events
// Copyright 2021 Richard L. Sites
//

// 2021.10.21 Redefine PSTATE as sample *after* the CPU frequency has changed (x86),
//            PSTATE2 as notify *before* the CPU frequency has changed (RPi4)
// TODO: determine if PSTATE2 applies to all cores or just the one.


// TODO: 
//	If send/receive on same machine, transmission will be faster than over Ether
//	- if tx.XYZW and then rx.XYZW, transmission time is no more than ts difference
//	-  in that case, tx and rx overlap in time and are short
//
//  move position of event to just after duration
//  accept exported names
//  upgrade ts and duration to uint64 instead of int
//  Ignore comments starting with #
//  Expand span to include arg retval name
//  change output to be json
// dick sites 2017.08.21
//  add mwait-implied spans
// dick sites 2017.08.21
//  Attach initial pid name to first entry's CPU stack
// dick sites 2017.11.18
//  add optional instructions per cycle IPC support
// dick sites 2018.06.14
//  Take another run at merging in PID names
// dick sites 2020.01.21
//  Reorganize around pre-process inserting extra events as needed
//
// dsites 2019.03.11
//  Grab PID from front of each traceblock
//  Move standalone return value into call span's return value
// dsites 2019.05.12
//  shorten AMD mwait to 13.3 usec
//  allow irq within BH
//  do not pop so soon on ctx inside sched
//  dummy push change to alter current span, back to its start
// dsites 2019.10.29
//  Add waiting spans
// dsites 2020.01.22
//  Rewrite and restructuring
// 2020.01.30 dsites Add PC samples
// 2020.02.01 dsites Add execution and profile aggregation
// 2020.02.04 dsites Have PC samples represent time before sample, not after
// 2020.07.13 dsites Have user process continue thru end of sched ctx switch
//                   to avoid bogus wait_* spans. Also see NestLevel
// 2020.07.13 dsites Address ambiguity of syscall/fault exit vs. block-resume
// 2020.08.19 dsites Add random traceid
// 2020.09.29 dsites Add lock lines
// 2020.11.01 dsites Turn raw filtered packet events into matching RPC packet spans
// 2020.11.06 dsites Fix corrupted idle stacked state at context switch
// 2020.11.13 dsites Added processing for RPC-to-packet correlation
// 2020.11.13 dsites Added codepoint for net speed, default to 1 Gbs
// 2021.01.26 dsites Redo RPC-to-packet correlation with hash16
// 2021.01.28 dsites Complete redo of RPC-to-packet correlation with hash32 again
// 2021.02.02 dsites Carry RPCid across ctx switch and back
// 2021.02.03 dsites Add queue names, enqueue/dequeue spans
// 2021.10.21 dsites Add pstate2 for Raspberry Pi
// 2021.10.22 dsites Chanfe mwait to wfi for Raspberry Pi

// Compile with  g++ -O2 eventtospan3.cc -o eventtospan3


/*TODO: 
  spantospan new ipc
  spantotrim new ipc
*/

#include <map>
#include <string>

#include <stdio.h>
#include <stdlib.h>     // exit, random
#include <string.h>
#include <time.h>
#include <unistd.h>     // getpid gethostname
#include <sys/time.h>   // gettimeofday
#include <sys/types.h>

#include "basetypes.h"
#include "kutrace_control_names.h"
#include "kutrace_lib.h"

// Event numbers or related masks
#define call_mask        0xc00
#define call_ret_mask    0xe00
#define ret_mask         0x200
#define type_mask        0xf00

// Names 001..1ff
// Point events 200..3ff
#define dummy_trap       0x4ff
#define dummy_irq        0x5ff
#define dummy_syscall    0x9ff
#define largest_non_pid  0xfff
#define pid_idle         0
#define event_idle       (0x10000 + pid_idle)
#define event_c_exit     0x20000

#define sched_syscall    0x9ff
#define sched_sysret     0xbff

#define ipc_mask         0x0f


// Additional drawing events
#define ArcNum       	-3

static const char* kIdleName = "-idle-";
static const char* kIdlelpName = "-idlelp-";
static const int kMAX_CPUS = 80;
static const int kNetworkMbitSec = 1000;	// Default: 1 Gb/s if not in trace

static const uint64 kMIN_CEXIT_DURATION = 10LL;	//  0.100 usec in multiples of 10 nsec
static const uint64 kMIN_WAIT_DURATION = 10LL;	//  0.100 usec in multiples of 10 nsec
static const uint64 kMAX_PLAUSIBLE_DURATION = 800000000LL;	// 8 sec in multiples of 10 nsec
static const uint64 kONE_MINUTE_DURATION =   6000000000LL;	// 60 sec in multiples of 10 nsec
static const uint64 kONE_HOUR =            360000000000LL;	// 3600 sec in multiples of 10 nsec

// We allow 26 waiting reasons, a-z, each displayed as Morse code
static const char* kWAIT_NAMES[26] = {
  "wait_a", "wait_b", "wait_cpu", "wait_disk",
  "wait_e", "wait_f", "wait_g", "wait_h",
  "wait_i", "wait_j", "wait_task", "wait_lock",
  "wait_mem", "wait_net", "wait_o", "wait_pipe",
  "wait_q", "wait_rcu", "wait_sche", "wait_time",
  "wait_u", "wait_v", "wait_w", "wait_x",
  "wait_y", "wait_unk", 
};

// ./drivers/idle/intel_idle.c
//   "C1-HSW",  0x00, .exit_latency = 2,        // times 100ns ?
//   "C1E-HSW", 0x01, .exit_latency = 10,
//   "C3-HSW",  0x10, .exit_latency = 33,
//   "C6-HSW",  0x20, .exit_latency = 133,
//   "C7s-HSW", 0x32, .exit_latency = 166,
//   "C8-HSW",  0x40, .exit_latency = 300,
//   "C9-HSW",  0x50, .exit_latency = 600,
//   "C10-HSW", 0x60, .exit_latency = 2500,

// Time for coming out of idle deep sleep
// Table entries are unspecified units; assume for the moment multiples of 100ns
static const int kLatencyTable[256] = {
    2, 10,  2,  2,   2,  2,  2,  2,   2,  2,  2,  2,   2,  2,  2,  2, 
   33, 33, 33, 33,  33, 33, 33, 33,  33, 33, 33, 33,  33, 33, 33, 33, 
  133,133,133,133, 133,133,133,133, 133,133,133,133, 133,133,133,133, 
  166,166,166,166, 166,166,166,166, 166,166,166,166, 166,166,166,166, 

  300,300,300,300, 300,300,300,300, 300,300,300,300, 300,300,300,300, 
  600,600,600,600, 600,600,600,600, 600,600,600,600, 600,600,600,600, 
  2500,2500,2500,2500, 2500,2500,2500,2500, 2500,2500,2500,2500, 2500,2500,2500,2500, 
  2500,2500,2500,2500, 2500,2500,2500,2500, 2500,2500,2500,2500, 2500,2500,2500,2500, 

  2500,2500,2500,2500, 2500,2500,2500,2500, 2500,2500,2500,2500, 2500,2500,2500,2500, 
  2500,2500,2500,2500, 2500,2500,2500,2500, 2500,2500,2500,2500, 2500,2500,2500,2500, 
  2500,2500,2500,2500, 2500,2500,2500,2500, 2500,2500,2500,2500, 2500,2500,2500,2500, 
  2500,2500,2500,2500, 2500,2500,2500,2500, 2500,2500,2500,2500, 2500,2500,2500,2500, 

  2500,2500,2500,2500, 2500,2500,2500,2500, 2500,2500,2500,2500, 2500,2500,2500,2500, 
  2500,2500,2500,2500, 2500,2500,2500,2500, 2500,2500,2500,2500, 2500,2500,2500,2500, 
  2500,2500,2500,2500, 2500,2500,2500,2500, 2500,2500,2500,2500, 2500,2500,2500,2500, 
  2500,2500,2500,2500, 2500,2500,2500,2500, 2500,2500,2500,2500, 2500,2500,   2, 133, 
  // [254] RPI4-B wfi() guess
  // [255] AMD mwait guess
};

// 2**0.0 through 2** 0.9
static const double kPowerTwoTenths[10] = {
  1.0000, 1.0718, 1.1487, 1.2311, 1.3195, 
  1.4142, 1.5157, 1.6245, 1.7411, 1.8661
};


using std::map;
using std::multimap;
using std::string;


// Per-PID short stack of events to return to.
// These are saved/restored when a thread, i.e. pid, is context switched out
// and later starts running again, possibly on another CPU.
//  stack[0] is always a user-mode pid
//  stack[1] is a system call or interrupt or fault
//  stack[2] and [3] are nested interrupts/faults
//  stack[4] can be scheduler
//
// +---------------+
// | ambiguous     |
// +---------------+
// | rpcid         |
// +---------------+
// | enque_num     |  
// +---------------+
// | deque_num     |
// +---------------+
// | top = 0..4    |
// +---------------+
// +---------------+    +-------------------------------+
// | eventnum      |    |  name                         | 0 user
// +---------------+    +-------------------------------+
// | eventnum      |    |  name                         | 1 syscall
// +---------------+    +-------------------------------+
// | eventnum      |    |  name                         | 2 fault
// +---------------+    +-------------------------------+
// | eventnum      |    |  name                         | 3 interrupt
// +---------------+    +-------------------------------+
// | eventnum      |    |  name                         | 4 scheduler
// +---------------+    +-------------------------------+
//
typedef struct {
  int ambiguous;		// Nonzero=True if scheduler runs within syscall/fault/IRQ
				// Not clear if scheduler exit returns or goes to user code
				// In this case, record subscript of ambiguous stack entry
				// See FixupAmbiguousSpan for details.
  int rpcid;			// Current RPC id for this PID. Overrides event.rpcid
  int enqueue_num_pending;	// For piecing together RPC waiting in a queue (-1 = inactive)
  int dequeue_num_pending;	// For piecing together RPC waiting in a queue (-1 = inactive)
  int top;		        // Top of our small stack
  int eventnum[5];		// One or more event numbers that are stacked calls
  string name[5];		// One or more event names that are stacked calls
} PidState;
         


// Event, from rawtoevent
// +-----+-----+-----+-----+-----+-----+-----+-----+-----+-----------+
// | ts  | dur | cpu | pid | rpc |event| arg | ret | ipc |  name     |
// +-----+-----+-----+-----+-----+-----+-----+-----+-----+-----------+
//

// Span, from start/end events
// After StartSpan
// +-----+-----+-----+-----+-----+-----+-----+-----+-----+-----------+
// | ts  | /// | cpu | pid | rpc |event| arg | ret | /// |  name     |
// +-----+-----+-----+-----+-----+-----+-----+-----+-----+-----------+
// After FinishSpan
// +-----+-----+-----+-----+-----+-----+-----+-----+-----+-----------+
// | ts  | dur | cpu | pid | rpc |event| arg | ret | ipc |  name     |
// +-----+-----+-----+-----+-----+-----+-----+-----+-----+-----------+
//
typedef struct {
  uint64 start_ts;	// Multiples of 10 nsec
  uint64 duration;	// Multiples of 10 nsec
  int cpu;
  int pid;
  int rpcid;		// For incoming events, this is bogus except in RPCIDREQ/RESP
  int eventnum;
  int arg;
  int retval;
  int ipc;
  string name;
} OneSpan;


// RPC correlation, one entry per pid
// k_timestamp is filled in by kernel TX_PKT (rpcid is known), else
//   by RX_USER (rpcid is known) copying from hash entry
// Using the PID, this accumulates three pieces from kernel/user/rpcid entries
typedef struct {
  uint64 k_timestamp;	// Time kernel code saw hash32. 0 means not known yet
  uint32 rpcid;		// 0 means not known yet
  uint16 lglen8;	// 0 means not known yet
  bool rx;		// true if rx 
} PidCorr; 

// RPC correlation, Packet or message hash to PID correlation, one entry per hash32
// Using the common hash32 value, this carries ts or pid between kernel/user packet entries
typedef struct {
  uint64 k_timestamp;	// Time kernel code saw hash32. 0 means not known yet
  uint32 pid;		// 0 means not known yet
} HashCorr;


// Contended-lock pending since ts with lock held by pid (-1 if unknown)
typedef struct {
  uint64 start_ts;
  int pid;
  int eventnum;
} LockContend;


// Per-CPU state: M sets of these for M CPUs
// +---------------+
// | cpu_stack   o-|--> current thread's PidState w/return stack
// +---------------+    saved and restored across context switches
//   cur_span:
// +-----+-----+-----+-----+-----+-----+-----+-----+-----+-----------+
// | ts  | dur | cpu | pid | rpc |event| arg | ret | ipc |  name     |
// +-----+-----+-----+-----+-----+-----+-----+-----+-----+-----------+
// +---------------+
// |prior_pstate_ts|
// +---------------+
// |      ... _freq|
// +---------------+
// |prior_pc_samp_t|
// +---------------+
// | ctx_switch_ts |
// +---------------+
// | mwait_pending |
// +---------------+
// | oldpid        |
// +---------------+
// | newpid        |
// +---------------+
// | valid_span    |
// +---------------+
//
typedef struct {
  PidState cpu_stack;		// Current call stack & span for each of kMAX_CPUS CPUs
  OneSpan cur_span;
  uint64 prior_pstate_ts;	// Used to assign duration to each pstate (CPU clock freq)
  uint64 prior_pstate_freq;	// Used to assign frequency to each pstate2 span
  uint64 prior_pc_samp_ts;	// Used to assign duration to each PC sample
  uint64 ctx_switch_ts;		// Used if /sched is missing
  int mwait_pending;		// eax value 00..FF, from mwait event.arg
  int oldpid;			// The pid on this CPU just before a context switch
  int newpid;			// The (current) pid on this CPU just after a context switch
				// Above two are used at scheduler exit if a wakeup of oldpid
				//  occurs *during* scheduling
  bool valid_span;		// Not valid span at beginning of trace
} CPUState;

//
// Globals across all CPUs
//
typedef map<int, PidState> PerPidState;	// State of each suspended task, by PID
typedef map<int, string> IntName;	// Name for each PID/lock/method
typedef map<int, OneSpan> PidWakeup;	// Previous wakeup event, by PID
typedef map<int, uint64> PidTime;	// Previous per-PID timestamp (span end, kernel-seen packet)
typedef map<int, uint> PidLock;		// Previous per-PID lock hash number
typedef map<int, uint> PidHash32;	// Previous per-PID pending user packet hash number
typedef map<int, bool> PidRunning;	// Set of currently-running PIDs
typedef map<uint64, LockContend> LockPending;	// Previous lock try&fail event, by lockhash&pid
						// Multiple threads can be wanting the same lock
typedef map<uint32, PidCorr> PidToCorr;		// pid to <timestamp, rpcid, len>
typedef map<uint32, HashCorr> HashToCorr;	// hash32 to <timestamp, pid>
typedef map<uint32, uint64> RpcQueuetime;	// rpcid to enqueue timestamp


// RPC-to-packet correlation
// 
// This elaborate-looking song-and-dance came about because I do not want
// the kernel code to know anything about dclab-specific RPC message formats,
// but do want to correlate the user-mode arrival of a message with the 
// first packet's arrival at kernel code. 
//
// The remaining packets of a long message are either not traced
// (based on the loadable module filter parameters), 
// or are traced and ignored here because they do not have
// a hash over the first 32 bytes that match a specific dclab RPC message.
//
// For each RPC, the trace has a METHODNAME entry:
//
// +-------------------+-----------+-------------------------------+
// | timestamp         | event     |             rpcid             | (0)
// +-------------------+-----------+-------------------------------+
// |  character name, 1-56 bytes, NUL padded                       |
// +- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -+
// ~                                                               ~
// +---------------------------------------------------------------+
//
// A trace may contain entries generated within the kernel TCP and UDP 
// processing that identify and timestamp selected packets. The entries
// are KUTRACE_RX_PKT and KUTRACE_TX_PKT with this format:
//
// +-------------------+-----------+---------------+-------+-------+
// | timestamp 1       | event     |             hash32            | (1)
// +-------------------+-----------+---------------+-------+-------+
//          20              12         8       8           16 
//
// The hash is a four-byte-wide XOR over the first 32 bytes of the packet payload
// starting at the byte just after the TCP or UDP header. Selected packets have
// at least 32 bytes of payload, and they pass a simple filter test over the
// first 24 bytes of the payload, based on 24 bytes of mask and 8 bytes of
// expected four-byte-wide XOR value after masking. The algorithm is compiled 
// into the kernel TCP and UDP code, but the mask and expected values are 
// supplied to the kutrace loadable module at its startup.
//
// The default filter picks off just packets that start with the dclab_rpc.h
// kMarkerSignature value. All packets and no packets are also choices, along
// with other combinations of user-specified byte masks and expected XOR value.
//
// The user-mode dclab RPC routines generate RPC request/response events. 
//
// The working-on-RPC events KUTRACE_RPCIDREQ and KUTRACE_RPCIDRESP have this 
// format:
// +-------------------+-----------+---------------+-------+-------+
// | timestamp 2       | event     |     lglen8    |     RPCid     | (2)
// +-------------------+-----------+---------------+-------+-------+
//          20              12         8       8           16 
//
// where lglen8 is ten times the log base 2 of the entire message length
// (messages are often multiple packets). A non-zero RPCid specifies working 
// on some RPC while zero specifies working on no RPC. 
//
// The first mention of an incoming RPC message may also have a hash32 value 
// taken over the first 32 bytes of the message -- the dclab RPC marker and 
// initial part of the RPC header. The entries are KUTRACE_RX_USER and 
// KUTRACE_TX_USER with the same format as the kernel entries above:
//
// +-------------------+-----------+---------------+-------+-------+
// | timestamp 3       | event     |             hash32            | (3)
// +-------------------+-----------+---------------+-------+-------+
//          20              12         8       8           16 
// 
// This value can be used with high probability to find a matching previous 
// RX_PKT or following TX_PKT entry (1), which would then give the time at which 
// the first packet of a message was processed in the kernel code. Combined with 
// the lglen value, this can be used to aproximate when the entire message 
// occupied the network link, shown by the synthetic KUTRACE_RPCIDRXMSG and 
// KUTRACE_RPCIDTXMSG text events created here:
//
//  TS         DUR EVENT   CPU PID RPC    ARG0 RETVAL IPC NAME 
//  timestamp1 dddd 516    0   0   rrrr   len  0      0   method.rrrr (204)
//
//  (ts and dur multiples of 10ns):
//  
// and where timestamp1 comes from the kernel KUTRACE_RX_PKT event (1), 
// the rpcid rrrr and message length len come from the user 
// KUTRACE_RPCIDREQ/RESP event (2), the method name comes from the usual 
// KUTRACE_METHODNAME event, and the duration dddd comes from a calculation 
// based on the message length and network bitrate NetworkMbPerSec which 
// currently just defaults to 1000 Mb/s but in the future will come from 
// the trace itself.
//
// If both tx and rx are on the same computer, no Ether is used, so transmission
// time is very fast, bounded by matching kernel tx and rx timestamps.
// The current code does not correct for this.
//
// Note that the input events (0) (1) (2) (3) may appear in the trace only in 
// the order described below, and may be created on different CPUs.
// The code caters to that. 
//
// There will be occasional duplicates of the 16-bit values involved.This will
// sometimes create false reconstructions of packet traffic.
//
// Expected events and their data
// a: rpcid
// b: method name
// c: length
// d: hash32
// e: kernel-seen timestamp
// f: pid
//
//  client request:  METHODNAME(ab), RPCIDREQ(acf), TX_USER(df), TX_PKT(de); put out TXMSG(abce)
//    server request:  RX_PKT(de), RX_USER(df), METHODNAME(ab), RPCIDREQ(acf); put out RXMSG(abce)
//    server response: RPCIDRESP(acf), TX_USER(df), TX_PKT(de); put out TXMSG(abce)
//  client response: RX_PKT(de), RX_USER(df), RPCIDRESP(acf); put out RXMSG(abce)
//
// 2021.01.28 BUGs
//  rx/tx widths wrong
//  packet length and position is correct for tx
//


// global queue names
IntName queuenames;		// small_int => queue name definitions
RpcQueuetime enqueuetime;	// rpcid => enqueue time

// RPC global method names
IntName methodnames;		// rpcid => method name definitions

// Pending RPC globals -- what we know about them so far. Transient across short sequences
// of the events above
PidToCorr pidtocorr;		// One process can only be doing one message RX/TX at once
HashToCorr rx_hashtocorr;	// Low-level Kernel/user can be doing multiple overlapping 
HashToCorr tx_hashtocorr;	//  packetsat once
static const PidCorr initpidcorr = {0, 0, 0, false};
static const HashCorr inithashcorr = {0, 0};

// Globals
bool verbose = false;
bool trace = false;
bool rel0 = false;
bool is_rpi = false;		// True for Raspberry Pi

string kernel_version;
string cpu_model_name;
string host_name;
int mbit_sec = kNetworkMbitSec;	// Default
int max_cpu_seen = 0;		// Keep track of how many CPUs there are


static uint64 span_count = 0;
static int incoming_version = 0;  // Incoming version number, if any, from ## VERSION: 2
static int incoming_flags = 0;    // Incoming flags, if any, from ## FLAGS: 128
IntName pidnames;		  // Current name for each PID, by pid# 
IntName pidrownames;		  // Collected names for each PID (clone, execve, etc. rename a thread), by pid#
PidWakeup pendingWakeup;	  // Any pending wakeup event, to make arc from wakeup to running
PidWakeup priorPidEvent;	  // Any prior event for each PID, to make wait_xxx display
PidTime priorPidEnd;	 	  // Any prior span end for each PID, to make wait_xxx display
PidLock priorPidLock;	 	  // Any prior lock hash number for each PID, to make wait_xxx display
IntName locknames;		  // Incoming lock name definitions
LockPending lockpending;	  // pending KUTRACE_LOCKNOACQUIRE/etc. events, by lock hash
PidWakeup pendingLock;	  	  // Any pending lock acquire event, to make wait_lock from try to acquire
PidTime pendingKernelRx;  	  // Any pending RX_PKT time, waiting for subsequent RPCID
PidRunning pidRunning;		  // Set of currently-running PIDs
				  // A PID is running from the /sched that context switches to it until
				  //  the /sched that context switches away from it. It is thus running
				  //  during all of that second context switch. Any wakeup delivered while
				  //  it is running creates no waiting before that wakeup.

// Stats
double total_usermode = 0.0;
double total_idle = 0.0;
double total_kernelmode = 0.0;
double total_other = 0.0;


// Fold 32-bit rpcid to 16-bit one
// 32-bit rpcid is never zero. If low bits are zero, use high bits
inline uint32 rpcid32_to_rpcid16(uint32 rpcid) {
  uint32 tempid = rpcid & 0xffff;
  return (tempid == 0) ? (rpcid >> 16) : tempid;
}

// Simple tests
uint64 uint64min(uint64 a, uint64 b) {return (a < b) ? a : b;}
uint64 uint64max(uint64 a, uint64 b) {return (a > b) ? a : b;}


// Events fall into five broad categories:
// (1) Text names for events
// (2) Point events
// (3) Kernel-mode execution
// (4) User-mode execution
// (5) Output-only HTML drawing events, not found in input raw binary traces

// (1) Any name definition
bool IsNamedef(int eventnum) {
  return (KUTRACE_VARLENLO <= eventnum) && (eventnum <= KUTRACE_VARLENHI);
}

// (2) Any point event 0x200 ..0x3FF
//     userpid, rpc, runnable, ipi, mwait, pstate, mark, lock, [not special:] pc, wait
bool IsAPointEvent(const OneSpan& event) {
  return ((KUTRACE_USERPID <= event.eventnum) && (event.eventnum < KUTRACE_TRAP));
}

// (3) Any kernel-mode execution event
bool IsKernelmode(const OneSpan& event) {
  return ((KUTRACE_TRAP <= event.eventnum) && (event.eventnum < event_idle));
}
bool IsKernelmodenum(int eventnum) {
  return ((KUTRACE_TRAP <= eventnum) && (eventnum < event_idle));
}

// (4) Any user-mode-execution event, in range 0x10000 .. 0x1ffff
// These includes the idle task
bool IsUserExec(const OneSpan& event) {
  return ((event.eventnum & 0xF0000) == 0x10000);
}
bool IsUserExecnum(int eventnum) {
  return ((eventnum & 0xF0000) == 0x10000);
}

bool IsCExitnum(int eventnum) {
  return (eventnum == 0x20000);
}

// True if the event means we must be executing in kernel mode
bool OnlyInKernelMode(const OneSpan& event) {
  if ((event.eventnum & 0xF00) == KUTRACE_TRAPRET) {return true;}	// Returns 6xx
  if ((event.eventnum & 0xF00) == KUTRACE_IRQRET) {return true;}	//         7xx
  if ((event.eventnum & 0xE00) == KUTRACE_SYSRET64) {return true;}	//         Axx, Bxx
  if ((event.eventnum & 0xE00) == KUTRACE_SYSRET32) {return true;}	//         Exx, Fxx
  if (event.eventnum == KUTRACE_USERPID) {return true;}		// context switch
  if (event.eventnum == KUTRACE_RUNNABLE) {return true;}	// make runnable
  if (event.eventnum == KUTRACE_IPI) {return true;}	// send IPI
  if (event.eventnum == KUTRACE_PSTATE) {return true;}	// current CPU clock frequency
  if (event.eventnum == KUTRACE_PSTATE2) {return true;}	// current CPU clock frequency
  if (event.eventnum == KUTRACE_PC_K) {return true;}	// kernel-mode PC sample in timer irq
  if (event.eventnum == KUTRACE_PC_U) {return true;}	// user-mode PC sample in timer irq 2020.11.06
  if (event.eventnum == sched_syscall) {return true;}	// only kernel can call the scheduler
  if (event.eventnum == sched_sysret) {return true;}	// only kernel can return from the scheduler
  return false;
}

// True if the event means we must be executing in user mode
bool OnlyInUserMode(const OneSpan& event) {
  if (event.eventnum == sched_syscall) {return false;}	// only kernel call the scheduler
  if ((event.eventnum & 0xE00) == KUTRACE_SYSCALL64) {return true;}	// Calls 8xx, 9xx
  if ((event.eventnum & 0xE00) == KUTRACE_SYSCALL32) {return true;}	// Calls Cxx, Dxx
  if (event.eventnum == KUTRACE_MWAIT) {return true;}	// mwait (in idle loop); so not inside call/irq/fault
  if (event.eventnum == KUTRACE_MARKA) {return true;}	// Marks [actually, these *could* be used in kernel]
  if (event.eventnum == KUTRACE_MARKB) {return true;}	// Marks
  if (event.eventnum == KUTRACE_MARKC) {return true;}	// Marks
  if (event.eventnum == KUTRACE_MARKD) {return true;}	// Marks
  return false;
}

// (5) Output-only HTML drawing events
// KUTRACE_WAITA .. KUTRACE_WAITZ for the various reasons a process or RPC waits


// Refinements ---------------------------------------------

// (1) Lock-name event, etc.
bool IsLockNameInt(int eventnum) {
  return ((eventnum & 0xF0F)== KUTRACE_LOCKNAME);
}
bool IsKernelVerInt(int eventnum) {
  return ((eventnum & 0xF0F) == KUTRACE_KERNEL_VER);
}
bool IsModelNameInt(int eventnum) {
  return ((eventnum & 0xF0F) == KUTRACE_MODEL_NAME);
}
bool IsHostNameInt(int eventnum) {
  return ((eventnum & 0xF0F) == KUTRACE_HOST_NAME);
}
bool IsMethodNameInt(int eventnum) {
  return ((eventnum & 0xF0F) == KUTRACE_METHODNAME);
}
bool IsQueueNameInt(int eventnum) {
  return ((eventnum & 0xF0F) == KUTRACE_QUEUE_NAME);
}
bool IsPidNameInt(int eventnum) {
  return ((eventnum & 0xF0F) == KUTRACE_PIDNAME);
}

// (2) UserPid point event
bool IsAContextSwitch(const OneSpan& event) {
  return (event.eventnum == KUTRACE_USERPID);
}

// (2) Make-runnable point event
bool IsAWakeup(const OneSpan& event) {
  return (event.eventnum == KUTRACE_RUNNABLE);
}

// (2) Mwait point event
bool IsAnMwait(const OneSpan& event) {
  return (event.eventnum == KUTRACE_MWAIT);
}

// (2) mark point event
bool IsAMark(const OneSpan& event) {
  return ((KUTRACE_MARKA <= event.eventnum) && (event.eventnum <= KUTRACE_MARKD));
}
// (2) lock point event 0x210 ..0x212
bool IsALockOneSpan(const OneSpan& event) {
  return ((KUTRACE_LOCKNOACQUIRE <= event.eventnum) && (event.eventnum <= KUTRACE_LOCKWAKEUP));
}
// (2) pstate point event
bool IsAPstate(const OneSpan& event) {
  return ((event.eventnum == KUTRACE_PSTATE) || (event.eventnum == KUTRACE_PSTATE2));
}
// (2) pc_sample point event
bool IsAPcSample(const OneSpan& event) {
  return ((event.eventnum == KUTRACE_PC_U) || (event.eventnum == KUTRACE_PC_K) || (event.eventnum == KUTRACE_PC_TEMP));
}
// (2) pc_sample point event
bool IsAPcSamplenum(int eventnum) {
  return ((eventnum == KUTRACE_PC_U) || (eventnum == KUTRACE_PC_K) || (eventnum == KUTRACE_PC_TEMP));
}

// (2) RPC point event: REQ RESP MID with optional lglen8
bool IsAnRpc(const OneSpan& event) {
  return ((KUTRACE_RPCIDREQ <= event.eventnum) && (event.eventnum <= KUTRACE_RPCIDMID));
}
// (2) RPC point event: REQ or RESP
bool IsRpcReqRespInt(int eventnum) {
  return ((eventnum == KUTRACE_RPCIDREQ) || (eventnum == KUTRACE_RPCIDRESP));
}
// (2) RPC network message, giving approximate time on the wire
bool IsAnRpcMsg(const OneSpan& event) {
  return ((KUTRACE_RPCIDRXMSG <= event.eventnum) && (event.eventnum <= KUTRACE_RPCIDTXMSG));
}
// (2) enque point event 
bool IsAnEnqueue(const OneSpan& event) {
  return (KUTRACE_ENQUEUE == event.eventnum);
}
// (2) deque point event 
bool IsADequeue(const OneSpan& event) {
  return (KUTRACE_DEQUEUE == event.eventnum);
}


// Return true if the event is raw kernel packet receive/send time and hash
bool IsRawPktHashInt(int eventnum) {
  return ((eventnum == KUTRACE_RX_PKT) || (eventnum == KUTRACE_TX_PKT));
}

// Kernel code sees a packet
bool IsRawRxPktInt(int eventnum) {return (eventnum == KUTRACE_RX_PKT);}
bool IsRawTxPktInt(int eventnum) {return (eventnum == KUTRACE_TX_PKT);}

// User code sees a packet
bool IsUserRxPktInt(int eventnum) {return (eventnum == KUTRACE_RX_USER);}
bool IsUserTxPktInt(int eventnum) {return (eventnum == KUTRACE_TX_USER);}

// Incoming RPC request/response. Prior RX_USER has set up pidtocorr[pid]
bool IsIncomingRpcReqResp(const OneSpan& event) {
  return IsRpcReqRespInt(event.eventnum) && (event.arg != 0) && 
    (pidtocorr.find(event.pid) != pidtocorr.end());
}

// Outgoing RPC request/response. No pending pidcorr[pid]
bool IsOutgoingRpcReqResp(const OneSpan& event) {
  return IsRpcReqRespInt(event.eventnum) && (event.arg != 0) && 
    (pidtocorr.find(event.pid) == pidtocorr.end());
}


// Return true if the event is user message receive/send time and hash
inline bool IsUserMsgHashInt(int eventnum) {
  return (KUTRACE_RX_USER <= eventnum) && (eventnum <= KUTRACE_TX_USER);
}

// (3)
bool IsACall(const OneSpan& event) {
  ////if (IsUserExec(event)) return false;
  if (largest_non_pid < event.eventnum) return false;
  if ((event.eventnum & call_mask) == 0) return false;
  if ((event.eventnum & ret_mask) != 0) return false;
  return true;
}

// (3)
bool IsAReturn(const OneSpan& event) {
  if (largest_non_pid < event.eventnum) return false;
  if ((event.eventnum & call_mask) == 0) return false;
  if ((event.eventnum & ret_mask) == 0) return false;
  return true;
}

bool IsACallOrReturn(const OneSpan& event) {
  if (largest_non_pid < event.eventnum) return false;
  if ((event.eventnum & call_mask) == 0) return false;
  return true;
}

// (3)
bool IsASyscallOrReturn(const OneSpan& event) {  // Must be a call/ret already
  if ((event.eventnum & call_mask) == KUTRACE_SYSCALL64) {return true;}
  if ((event.eventnum & call_mask) == KUTRACE_SYSCALL32) {return true;}
  return false;
}

// (3)
bool IsOptimizedCall(const OneSpan& event) {  // Must be a call already
  return (event.duration > 0);
}

// (3)
// These syscalls return a pid_t of a new runnable task
// Catches both optimized calls and standalone return
bool IsNewRunnablePidSyscall(const OneSpan& event) {
  if (!IsACallOrReturn(event)) {return false;}
  if (!IsASyscallOrReturn(event)) {return false;}
  if (event.name == "clone") {return true;}
  if (event.name == "/clone") {return true;}
  if (event.name == "fork") {return true;}
  if (event.name == "/fork") {return true;}
  return false;
}


// (3)
bool IsSchedCallEvent(const OneSpan& event) {
  return (event.eventnum == sched_syscall);
}
bool IsSchedCallEventnum(int eventnum) {
  return (eventnum == sched_syscall);
}

bool IsSchedReturnEvent(const OneSpan& event) {
  return (event.eventnum == sched_sysret);
}
bool IsSchedReturnEventnum(int eventnum) {
  return (eventnum == sched_sysret);
}


// (4)
bool IsAnIdle(const OneSpan& event) {
  return (event.eventnum == event_idle);
}
bool IsAnIdlenum(int eventnum) {
  return (eventnum == event_idle);
}

// (4) These exclude the idle task
bool IsUserExecNonidle(const OneSpan& event) {
  return ((event.eventnum & 0xF0000) == 0x10000) && !IsAnIdle(event);
}
bool IsUserExecNonidlenum(int eventnum) {
  return ((eventnum & 0xF0000) == 0x10000) && !IsAnIdlenum(eventnum);
}

// (Other)
// Events that contribute to CPU execution time, including idle
// Also including RPC begin/middle/end markers
bool IsExecContrib(const OneSpan& event) {
  if (event.duration < 0) {return false;}
  if (KUTRACE_TRAP <= event.eventnum) {return true;}
  if (IsAnRpc(event)) {return true;}
  return false;
}

// Events that contribute to profile: waits and PC samples
bool IsProfContrib(const OneSpan& event) {
  if (event.duration < 0) {return false;}
  if ((KUTRACE_PC_U <= event.eventnum) && (event.eventnum <= KUTRACE_PC_K)) {return true;}
  if ((KUTRACE_WAITA <= event.eventnum) && (event.eventnum <= KUTRACE_WAITZ)) {return true;}
  return false;
}


// End Refinements -----------------------------------------

// Convert ten * lg(x) back into x
uint64 TenPow(uint8 xlg) {
  int powertwo = xlg / 10;
  int fraction = xlg % 10;
  uint64 retval = 1llu << powertwo;
  retval = (retval * kPowerTwoTenths[fraction]) + 0.5;
  return retval;
}

// Convert message byte length to approximate usec on the wire
inline uint64 msg_dur_usec(uint64 length) {
  return (length * 8) / mbit_sec;
}

// Convert message byte length to approximate multiple of 10 nsec on the wire
inline uint64 msg_dur_10nsec(uint64 length) {
  return (length * 800) / mbit_sec;
}

// Clean away any non-Ascii characters
void Clean(string* s) {
  for (int i = 0; i < s->length(); ++i) {
    char c = (*s)[i];
    if ((c < ' ') || ('~' < c)) {
      (*s)[i] = '?';
    }
  }
}

string IntToString(int x) {
  char temp[24];
  sprintf(temp, "%d", x);
  return string(temp); 
}


// A user-mode-execution event is the pid number plus 64K
int PidToEventnum(int pid) {return (pid & 0xFFFF) + 0x10000;}
int EventnumToPid(int eventnum) {return eventnum & 0xFFFF;}

// Format a user thread name
string NameAppendPid(string name, int pid) {
  if (pid == 0) {return kIdleName;}
  return name + "." + IntToString(pid);
}

// Initially empty stack of -idle- running on this thread
void InitPidState(PidState* t) {
  t->ambiguous = 0;
  t->rpcid = 0;
  t->enqueue_num_pending = -1;	// No active partial RPC
  t->dequeue_num_pending = -1;	// No active partial RPC
  t->top = 0;
  for (int i = 0; i < 5; ++i) {
    t->eventnum[i] = event_idle;
    t->name[i].clear();
    t->name[i] = string(kIdleName);
  }
}

void BrandNewPid(int newpid, const string& newname, PerPidState* perPidState) {
  PidState temp;
  InitPidState(&temp);
  temp.top = 1;
  temp.eventnum[0] = PidToEventnum(newpid);
  temp.name[0] = newname;
  // Use current name, not the possibly-bad one from rawtoevent
  if (pidnames.find(newpid) != pidnames.end()) {
    temp.name[0] = NameAppendPid(pidnames[newpid], newpid);
  }
  temp.eventnum[1] = sched_syscall;
  temp.name[1] = "-sched-";
  (*perPidState)[newpid] = temp;
}



// Initially -idle- running on this CPUg
void InitSpan(OneSpan* s, int i) {
  memset(s, 0, sizeof(OneSpan));	// Takes care of commented zeros below
  // s->start_ts = 0;
  // s->duration = 0;
  s->cpu = i;
  s->pid = pid_idle;
  // s->rpcid = 0;
  s->eventnum = event_idle;
  // s->arg = 0;	// idle(0) regular; idle(1) low-power after mwait
  // s->retval = 0;
  // s->ipc = 0;
  s->name = kIdleName;
}

// Example:
// [ 49.7328170, 0.0000032, 0, 0, 0, 1519, 0, 0, "local_timer_vector"],



// Spans are dumped as < ... >
// Events as [ ... ]
// Stacks as { ... }

void DumpSpan(FILE* f, const char* label, const OneSpan* span) {
  fprintf(f, "%s <%llu %llu %d  %d %d %d %d %d %d %s>\n", 
  label, span->start_ts, span->duration, span->cpu, 
  span->pid, span->rpcid, span->eventnum, span->arg, span->retval, span->ipc, span->name.c_str());
}

void DumpSpanShort(FILE* f,  const OneSpan* span) {
  fprintf(f, "<%llu %llu ... %s> ", span->start_ts, span->duration, span->name.c_str());
}

void DumpStack(FILE* f, const char* label, const PidState* stack) {
  fprintf(f, "%s [%d] %d %d {\n", label, stack->top, stack->ambiguous, stack->rpcid);
  for (int i = 0; i < 5; ++i) {
    fprintf(f, "  [%d] %05x %s\n",i, stack->eventnum[i], stack->name[i].c_str());
  }
  fprintf(f, "}\n");
}

void DumpStackShort(FILE* f, const PidState* stack) {
  fprintf(f, "%d{", stack->top);
  for (int i = 0; i <= stack->top; ++i) {
    fprintf(f, "%s ", stack->name[i].c_str());
  }
  fprintf(f, "}%s %d ", stack->ambiguous ? "ambig" : "", stack->rpcid);
}

void DumpEvent(FILE* f, const char* label, const OneSpan& event) {
  fprintf(f, "%s [%llu %llu %d  %d %d %d %d %d %d %s]\n", 
  label, event.start_ts, event.duration, event.cpu, 
  event.pid, event.rpcid, event.eventnum, event.arg, event.retval, event.ipc, event.name.c_str());
}


// Complain if more than 60 seconds
bool CHECK(const char* lbl, const OneSpan& item) {
  bool error = false;
  if (item.start_ts > kONE_HOUR) {error = true;}
  if (item.duration > kONE_MINUTE_DURATION) {error = true;}
  if (item.start_ts + item.duration > kONE_HOUR) {error = true;}
  if (error) {fprintf(stderr, "%s ", lbl); DumpEvent(stderr, "****CHECK ", item);}
  return error;
}



string MaybeExtend(string s, int x) {
  string maybe = "." + IntToString(x);
  if (s.find(maybe) == string::npos) {return s + maybe;}
  return s;
}



// Return floor of log base2 of x, i.e. the number of bits-1 needed to hold x
int FloorLg(uint64 x) {
  int lg = 0;
  uint64 local_x = x;
  if (local_x & 0xffffffff00000000LL) {lg += 32; local_x >>= 32;}
  if (local_x & 0xffff0000LL) {lg += 16; local_x >>= 16;}
  if (local_x & 0xff00L) {lg += 8; local_x >>= 8;}
  if (local_x & 0xf0LL) {lg += 4; local_x >>= 4;}
  if (local_x & 0xcLL) {lg += 2; local_x >>= 2;}
  if (local_x & 0x2LL) {lg += 1; local_x >>= 1;}
  return lg;
}


// Close off the current span
// Remember each user-mode PID end in priorPidEnd
void FinishSpan(const OneSpan& event, OneSpan* span) {
  // Prior span duration is up until new event timestamp
  span->duration = event.start_ts - span->start_ts;

  // CHECK NEGATIVE or TOO LRAGE
  if (span->duration > kMAX_PLAUSIBLE_DURATION) {	// 8 sec in 10 nsec increments
    // Too big to be plausible with timer interrupts every 10 msec or less,
    // Except wait_* events can be very long
    // Force short positive
    span->duration = 1;			// 10 nsec

    if (event.start_ts < span->start_ts) {
      // Force negative span to short positive
      //fprintf(stderr, "BUG %llu .. %llu, duration negative, %lld0ns\n", 
      //        span->start_ts, event.start_ts, span->duration);
    } else {
      // Force big positive span to medium positive
      // except, ignore spans starting at 0
      if (span->start_ts != 0) {
        fprintf(stderr, "BUG %llu .. %llu, duration too big %lld\n", 
                span->start_ts, event.start_ts, span->duration);
        span->duration = 1000000;	// 10 msec
      }
    }
  }

// Need to match up return value with call
// 5330796455 0 2049 3  4052 0  1 0 0 write (801)    <=== arg0, no retval
// 5330796988 1 518 3  4052 0  3644 0 0 runnable (206)
// 5330797239 1 519 3  4052 0  1 0 0 sendipi (207)
// 5330797466 0 2561 3  4052 0  0 12 0 /write (a01)  <=== retval: write(1)=12

  // For IsOptimizedCall call/ret, entire span was already consumed
  // For unoptimized, span's return value is in the ending event
  if (IsAReturn(event)) {
    span->retval = event.retval;
  }

  // This span's ipc is in  the ending event
  span->ipc = event.ipc & ipc_mask;

  // Remember the end of last instance of each PID user-mode execution
  if ((span->pid > 0) && (span->cpu >= 0) /* && IsUserExecnum(span->eventnum) */ ) {
    priorPidEnd[span->pid] = span->start_ts + span->duration;
  }
}

// Open up a new span
void StartSpan(const OneSpan& event, OneSpan* span) {
  span->start_ts = event.start_ts;
  span->duration = 0;
  span->cpu = event.cpu;
  span->pid = event.pid;
  span->rpcid = event.rpcid;
  span->eventnum = event.eventnum;
  span->arg = event.arg;
  span->retval = event.retval;
  span->ipc = 0;
  span->name = event.name;
  // Clean up any user span with leftover arg/ret
  // Clean up uer-mode thread name
  if (IsUserExecnum(event.eventnum)) {
    span->arg = 0;
    span->retval = 0;
  }
}

void MakeArcSpan(const OneSpan& event1, const OneSpan& event2, OneSpan* span) {
  span->start_ts = event1.start_ts;
  span->duration = event2.start_ts - event1.start_ts;
  span->cpu = event1.cpu;
  span->pid = event1.pid;
  span->rpcid = event1.rpcid;
  span->eventnum = ArcNum;
  span->arg = event2.cpu;
  span->retval = event2.pid;	// Added 2020.08.20
  span->ipc = 0;
  span->name = "-wakeup-";
}

// Waiting on reason c from event1 to event2. For PID or RPC, not on any CPU
// Make a waiting span: letter, time, dur, CPU -1, PID, RPC, name
// letter is 'a' through 'z'
// start time is from whenever the given PID was last running
// end time is from whenever the given PID runs again
// RPCid is from whenever the given PID runs again
// name is cpu, disk, net, etc. for generic waits
// name is disk_sdb1 if specific disk is available
// name is lock_ffffff:llll if specific lock source filename and line are available
//
// For PID and RPC only; not CPU-specific
void MakeWaitSpan(char letter, uint64 start_ts, uint64 end_ts, int pid, int rpcid, OneSpan* span) {
  // Start 10 nsec late, so that HTML searches can find non-wait event
  span->start_ts = start_ts + 1;		// last execution of PID,or wakeup time
  span->duration = end_ts - start_ts - 1;	// next execution of PID
  if (start_ts == end_ts) {span->duration = 0;}	// We will throw this span away
  span->cpu = -1;
  span->pid = pid;
  span->rpcid = rpcid;
  if (letter < 'a') {letter = 'a';}
  if ('z' < letter) {letter = 'z';}
  span->eventnum = KUTRACE_WAITA + (letter - 'a');
  span->arg = 0;
  span->retval = 0;
  span->ipc = 0;
  span->name = kWAIT_NAMES[letter - 'a'];
}

// For PID only; not CPU- or RPC-specific
void MakeLockSpan(bool dots, uint64 start_ts, uint64 end_ts, int pid, 
                  int lockhash, const string& lockname, OneSpan* span) {
  span->start_ts = start_ts;
  span->duration = end_ts - start_ts;
  span->cpu = -1;
  span->pid = pid;
  span->rpcid = -1;
  span->eventnum = dots ? KUTRACE_LOCK_TRY : KUTRACE_LOCK_HELD;
  span->arg = lockhash;
  span->retval = 0;
  span->ipc = 0;
  span->name = lockname;
}

// To insert just after context switch back to a preempted in-progress RPC
// For all of CPU, PID, and RPC 
void MakeRpcidMidSpan(uint64 start_ts, int cpu, int pid, int rpcid, OneSpan* span) {
  char rpc_name[64];
  sprintf(rpc_name, "%s.%d", methodnames[rpcid].c_str(), rpcid);

  span->start_ts = start_ts;
  span->duration = 1;
  span->cpu = cpu;
  span->pid = pid;
  span->rpcid = rpcid;
  span->eventnum = KUTRACE_RPCIDMID;
  span->arg = rpcid;
  span->retval = 0;
  span->ipc = 0;
  span->name = string(rpc_name);
}

// To insert just after dequeuing an RPC
// For RPC only; not CPU- or PID-specific
void MakeQueuedSpan(uint64 start_ts, uint64 end_ts, int queue_num, int rpcid, OneSpan* span) {
  span->start_ts = start_ts;
  span->duration = end_ts - start_ts;
  span->cpu = -1;
  span->pid = -1;
  span->rpcid = rpcid;
  span->eventnum = KUTRACE_ENQUEUE;
  span->arg = queue_num;
  span->retval = 0;
  span->ipc = 0;
  span->name = string(queuenames[queue_num]);
}


// If we turned the current span idle into c_exit, now put it back
// OBSOLETE. unused
void CexitBackToIdle(OneSpan* span) {
  if (span->eventnum != event_c_exit) {return;}
  span->eventnum = event_idle;
  span->name = string(kIdleName);
//fprintf(stdout, "CexitBackToIdle at %llu\n", span->start_ts);
}

// Make sure bugs about renaming the idle pid are gone. DEFUNCT
void CheckSpan(const char* label, const CPUState* thiscpu) {
  bool fail = false;
  const OneSpan* span = &thiscpu->cur_span;
  if ((span->name == string(kIdleName)) && 
      (span->eventnum != event_idle)) {fail = true;}
  for (int i = 0; i < 5; ++i) {
    if ((thiscpu->cpu_stack.name[i] == string(kIdleName)) && 
        (thiscpu->cpu_stack.eventnum[i] != event_idle)) {fail = true;}
  }
  if (fail) {
    fprintf(stderr, "\nCheckSpan failed ==================================\n");
    fprintf(stdout, "\nCheckSpan failed ==================================\n");
    DumpSpan(stdout, label, span);
    DumpStack(stdout, label, &thiscpu->cpu_stack);
  }
}

// Write the current timespan and start a new one
// Change time from multiples of 10ns to seconds
// ts           dur       CPU tid  rpc event arg0 ret  name
// Make time stamp ts 12.8 so fixed width for later text sort
void WriteSpanJson2(FILE* f, const OneSpan* span) {
  if (span->start_ts == 0) {return;}       // Front of trace for each CPU
  if (span->duration > kMAX_PLAUSIBLE_DURATION) {return;}  // More than 8 sec in 10ns increments

  // Output
  // time dur cpu pid rpcid event arg retval ipc name
  // Change time from multiples of 10 nsec to seconds and fraction
  double ts_sec = span->start_ts / 100000000.0;
  double dur_sec = span->duration / 100000000.0;
//CHECK("f", *span);
  //                   ts dur cpu  pid rpc event  arg ret ipc  name
  fprintf(f, "[%12.8f, %10.8f, %d, %d, %d, %d, %d, %d, %d, \"%s\"],", 
          ts_sec, dur_sec, span->cpu, 
          span->pid, span->rpcid, span->eventnum, 
          span->arg, span->retval, span->ipc, span->name.c_str());
  ++span_count;
  fprintf(f, "\n");
 
  // Stastics
  if (IsUserExecNonidlenum(span->eventnum)) {
    total_usermode += dur_sec;
  } else if (IsAnIdlenum(span->eventnum)) {
    total_idle += dur_sec;
  } else if (IsKernelmodenum(span->eventnum)) {
    total_kernelmode += dur_sec;
  } else {
    total_other += dur_sec;
  }
}

void WriteSpanJson(FILE* f, const CPUState* thiscpu) {
  WriteSpanJson2(f, &thiscpu->cur_span);
}

// Write a point event, so they aren't lost
// Change time from multiples of 10 nsec to seconds and fraction
void WriteEventJson(FILE* f, const OneSpan* event) {
  double ts_sec = event->start_ts / 100000000.0;
  double dur_sec = event->duration / 100000000.0;
//CHECK("g", *event);
  //                   ts dur cpu  pid rpc event  arg ret ipc  name
  fprintf(f, "[%12.8f, %10.8f, %d, %d, %d, %d, %d, %d, %d, \"%s\"],\n", 
          ts_sec, dur_sec, event->cpu, 
          event->pid, event->rpcid, event->eventnum,
          event->arg, event->retval, event->ipc, event->name.c_str());
  ++span_count;
}

// Open the json variable and give inital values
void InitialJson(FILE* f, const char* label, const char* basetime) {
  // Generate a pseudo-ranndom trace ID number, in case we need to distinguish
  // saved state for different traces beyond their basetime
  unsigned int randomid = (time(NULL) ^ (getpid() * 12345678)) & 0x7FFFFFFF;

  // Leading spaces are to keep this all in front and in order after text sort
  fprintf(f, "  {\n");
  fprintf(f, " \"Comment\" : \"V2 with IPC field\",\n");
  fprintf(f, " \"axisLabelX\" : \"Time (sec)\",\n");
  fprintf(f, " \"axisLabelY\" : \"CPU Number\",\n");
  fprintf(f, " \"flags\" : %d,\n", incoming_flags);
  fprintf(f, " \"randomid\" : %d,\n", randomid);
  fprintf(f, " \"shortUnitsX\" : \"s\",\n");
  fprintf(f, " \"shortMulX\" : 1,\n");
  fprintf(f, " \"thousandsX\" : 1000,\n");
  fprintf(f, " \"title\" : \"%s\",\n", label);
  fprintf(f, " \"tracebase\" : \"%s\",\n", basetime);
  fprintf(f, " \"version\" : %d,\n", incoming_version);
  if (!kernel_version.empty()) {
    Clean(&kernel_version);
    fprintf(f, " \"kernelVersion\" : \"%s\",\n", kernel_version.c_str());
  }
  if (!cpu_model_name.empty()) {
    Clean(&cpu_model_name);
    fprintf(f, " \"cpuModelName\" : \"%s\",\n", cpu_model_name.c_str());
  }
  if (!host_name.empty()) {
    Clean(&host_name);
    fprintf(f, " \"hostName\" : \"%s\",\n", host_name.c_str());
  }

  fprintf(f, "\"events\" : [\n");
}

// Add dummy entry that sorts last, then close the events array and top-level json
void FinalJson(FILE* f) {
  fprintf(f, "[999.0, 0.0, 0, 0, 0, 0, 0, 0, 0, \"\"]\n");	// no comma
  fprintf(f, "]}\n");
}

// Design for push/pop of nested kernel routines
// Each per-CPU stack, thiscpu->cpu_stack, keeps track of routines that have been
// entered but not yet exited. In general, entry/exit events in the trace can be
// missing or unbalanced. At the beginning of a trace, there can be early exit 
// events with no prior entry. Some events exit abnormally with no explicit
// exit event. Events are somewhat constrained in thier nesting, which helps 
// track what must be going on. NestLevel reflects these constraints.
// If A is on the stack and a call to B occurs, the NestLevel of B should be
// greater than the NesdtLevel of A. If not, a synthetic pop of A is inserted.
// Conversely, if A is on the stack and a return from B occurs, a synthetic
// call to B is inserted. If A and B (top) are stacked and a return from A occurs,
// a synthetic return from B is inserted. 
//
// In particular, there is an ambiguity for syscalls. Some return normally with 
// a sysret event. Others exit directly though the scheduler with no sysret. 
// Others block, go thru the scheduler, then later resume and exit normally 
// with a sysret event (or block multiple times before normal exit). There is 
// not enough information in the raw trace to distinguish immediate return from
// blocking with later return. With process A running, we expect the sequence
//   syscall C, sysret C
// but if instead we get
//   syscall C, sched, ctxswitch to B, /sched
// we leave the syscall on the stack but mark the stack ambiguous. This stack 
// is saved for PID=A. Later, when we encounter a context switch back to process A,
//   ..., sched, ctxswitch to A, /sched, N
// the A stack is restored at ctxswitch, but we don't know whether exiting the 
// scheduler goes to suspended syscall C or all the way out to user code A.
//
// We resolve the ambiguity by setting up the span after sched, but don't commit to 
// what is running in that span until we reach the next event N. If N is something 
// that can only be done in kernel mode (sched, runnable, kernel PC sample, user PID,
// sendIPI, mwait, freq, sysret) we remove the ambiguous mark and set the span to 
// the syscall. If  N is something that can only be done in user mode (user PC
// sample, syscall) we remove the ambiguous mark, pop the stack, and set the 
// span to user process A. If neither occurs (e.g. a disk interrupt), we leave 
// the stack ambiguous but default the span to be user process A. (We could 
// conceivably create a new span type "ambiguous" and perhaps display it as gray.)
// In this last case, the ambiguity will eventually be resolved later. If we
// had a multi-pass eventtospan design, we could go back and fix up the ambiguous
// spans. But this is really getting to be an edge case.

// Nesting levels are user:0, syscall:1, trap:2, IRQ:3, sched_syscall:4.
// It is only legal to call to a numerically larger nesting level
// Note that we can skip levels, so these are not exactly stack depth
int NestLevel(int eventnum) {
  if (largest_non_pid < eventnum) {return 0;}			// User-mode pid
  if (eventnum == sched_syscall) {return 4;}			// Enter the scheduler
								// must precede syscall below
  if ((eventnum & call_ret_mask) == KUTRACE_SYSCALL64) {return 1;}	// syscall/ret
  if ((eventnum & call_ret_mask) == KUTRACE_SYSCALL32) {return 1;}	// syscall/ret
  if ((eventnum & type_mask) == KUTRACE_TRAP) {return 2;}		// trap
  if ((eventnum & type_mask) == KUTRACE_IRQ) {return 3;}		// interrupt
  return 1;	// error; pretend it is a syscall
}

// This deals with mis-nested call
void AdjustStackForPush(const OneSpan& event, CPUState* thiscpu) {
  while (NestLevel(event.eventnum) <= 
         NestLevel(thiscpu->cpu_stack.eventnum[thiscpu->cpu_stack.top])) {
fprintf(stdout,"AdjustStackForPush FAIL\n");
    // Insert dummy returns, i.e. pop, until the call is legal or we are at user-mode level
    if (thiscpu->cpu_stack.top == 0) {break;}
if (verbose) fprintf(stdout, "-%d  dummy return from %s\n", 
event.cpu, thiscpu->cpu_stack.name[thiscpu->cpu_stack.top].c_str());
    --thiscpu->cpu_stack.top;
  }
}

// This deals with unbalanced return
void AdjustStackForPop(const OneSpan& event, CPUState* thiscpu) {
  if (thiscpu->cpu_stack.top == 0) {
fprintf(stdout,"AdjustStackForPop FAIL\n");
    // Trying to return above user mode. Push a dummy syscall
if (verbose) fprintf(stdout, "+%d dummy call to %s\n", event.cpu, event.name.c_str());
    ++thiscpu->cpu_stack.top;
    thiscpu->cpu_stack.eventnum[thiscpu->cpu_stack.top] = dummy_syscall;
    thiscpu->cpu_stack.name[thiscpu->cpu_stack.top] = string("-dummy-");
  }
  // If returning from something lower nesting than top of stack,
  // pop the stack for a match. 
  int matching_call = event.eventnum & ~ret_mask;		// Turn off the return bit
  while (NestLevel(matching_call) < 
         NestLevel(thiscpu->cpu_stack.eventnum[thiscpu->cpu_stack.top])) {
fprintf(stdout,"AdjustStackForPop FAIL\n");
    // Insert dummy returns, i.e. pop, until the call is legal or we are at user-mode level
    if (thiscpu->cpu_stack.top == 1) {break;}
if (verbose) fprintf(stdout, "-%d  dummy return from %s\n", 
event.cpu, thiscpu->cpu_stack.name[thiscpu->cpu_stack.top].c_str());
    --thiscpu->cpu_stack.top;
  }
}

// Add the pid# to the end of user-mode name, if not already there
string AppendPid(const string& name, uint64 pid) {
  char pidnum_temp[16];
  sprintf(pidnum_temp, ".%lld", pid & 0xffff);
  if (strstr(name.c_str(), pidnum_temp) == NULL) {
    return name + string(pidnum_temp);
  }
  return name;
}

string EventNamePlusPid(const OneSpan& event) {
  return AppendPid(event.name, event.pid); 
}

void DumpShort(FILE* f, const CPUState* thiscpu) {
  fprintf(f, "\t");
  DumpStackShort(f, &thiscpu->cpu_stack);
  fprintf(f, "\t");
  DumpSpanShort(f, &thiscpu->cur_span);
  fprintf(f, "\n");
}

// Insert wait_* span for reason that we were waiting
void WaitBeforeWakeup(const OneSpan& event, CPUState* cpustate, PerPidState* perPidState) {
  CPUState* thiscpu = &cpustate[event.cpu];
  int target_pid = event.arg;

  // The wakeup has a target PID. We keep a list of the most recent user-mode event 
  // mentioning that PID, if any. The time from last mention to now is the
  // waiting time; the current wakeup event signals the end of that waiting.
  // The top of the per-CPU call stack says what kernel routine is doing the wakeup.
  // TRICKY: The target PID might actually be running or in the scheduler right now, 
  // about to be context switched out. Inthat case, avoid any before-wakeup event.

  // There is no priorPidEvent at the beginning of a trace. 
  if (priorPidEvent.find(target_pid) == priorPidEvent.end()) {return;}

  // If the target PID is currently executing, do not generate a wait
  if (pidRunning.find(target_pid) != pidRunning.end()) {return;}

  OneSpan& old_event = priorPidEvent[target_pid];
  const PidState* stack = &thiscpu->cpu_stack;

  // Create wait_* events
  // Also see soft_irq_name in rawtoevent.cc
  char letter = ' ';		// Default = unknown reason for waiting
  if (stack->name[stack->top] == "local_timer_vector") {	// timer
    letter = 't';		// timer
  } else if (stack->name[stack->top] == "arch_timer") {		// Rpi time
    letter = 't';		// timer
  } else if (stack->name[stack->top] == "page_fault") {	// memory
    letter = 'm';		// memory
  } else if (stack->name[stack->top] == "mmap") {
    letter = 'm';		// memory
  } else if (stack->name[stack->top] == "munmap") {
    letter = 'm';		// memory
  } else if (stack->name[stack->top] == "mprotect") {
    letter = 'm';		// memory
  } else if (stack->name[stack->top] == "futex") {	// lock
    letter = 'l';		// lock
  } else if (stack->name[stack->top] == "writev") {	// pipe
    letter = 'p';		// pipe
  } else if (stack->name[stack->top] == "write") {
    letter = 'p';		// pipe
  } else if (stack->name[stack->top] == "sendto") {
    letter = 'p';		// pipe
  } else if (stack->name[stack->top].substr(0,7) == "kworker") {
    letter = 'p';		// pipe
  } else if (stack->name[stack->top] == "BH:hi") {	// tasklet
    letter = 'k';		// high prio tasklet or unknown BH fragment
  } else if (stack->name[stack->top] == "BH:timer") {	// time
    letter = 't';		// timer
  } else if (stack->name[stack->top] == "BH:tx") {	// network
    letter = 'n';		// network
  } else if (stack->name[stack->top] == "BH:rx") {
    letter = 'n';		// network
  } else if (stack->name[stack->top] == "BH:block") {	// disk
    letter = 'd';		// disk/SSD
  } else if (stack->name[stack->top] == "BH:irq_p") {
    letter = 'd';		// disk/SSD (iopoll)
  } else if (stack->name[stack->top] == "syncfs") {
    letter = 'd';		// disk/SSD 
  } else if (stack->name[stack->top] == "BH:taskl") {
    letter = 'k';		// normal tasklet
  } else if (stack->name[stack->top] == "BH:sched") {	// sched
    letter = 's';		// scheduler (load balancing)
  } else if (stack->name[stack->top] == "BH:hrtim") {
    letter = 't';		// timer
  } else if (stack->name[stack->top] == "BH:rcu") {
    letter = 't';		// read-copy-update release code
  }

  if ((letter != ' ')) {
    // Make a wait_* display span
    OneSpan temp_span = thiscpu->cur_span;	// Save
    MakeWaitSpan(letter, priorPidEnd[target_pid], 
      event.start_ts, target_pid, old_event.rpcid, &thiscpu->cur_span);

    // Don't clutter if the waiting is short (say < 10 usec)
    if (thiscpu->cur_span.duration >= kMIN_WAIT_DURATION) {
      WriteSpanJson(stdout, thiscpu);	// Standalone wait_cpu span
    }
    thiscpu->cur_span = temp_span;		// Restore
  }
}

void WaitAfterWakeup(const OneSpan& event, CPUState* cpustate, PerPidState* perpidstate) {
  CPUState* thiscpu = &cpustate[event.cpu];
  int target_pid = event.arg;
}

void DoWakeup(const OneSpan& event, CPUState* cpustate, PerPidState* perpidstate) {
  CPUState* thiscpu = &cpustate[event.cpu];
  int target_pid = event.arg;
  // Remember the wakeup
  pendingWakeup[target_pid] = event;

  // Any subsequent waiting will be for CPU, starting at this wakeup
  priorPidEnd[target_pid] = event.start_ts + event.duration;
}

void SwapStacks(int oldpid, int newpid, const string& name, CPUState* thiscpu, PerPidState* perpidstate) {
  if (oldpid == newpid) {return;}

  // Swap out the old thread's stack, but don't change the idle stack
  if (oldpid != 0) {
    (*perpidstate)[oldpid] = thiscpu->cpu_stack;
  }
if (verbose) {
fprintf(stdout, "SwapStacks old %d: ", oldpid);
DumpStackShort(stdout, &thiscpu->cpu_stack);
}
  if (perpidstate->find(newpid) == perpidstate->end()) {
    // Switching to a thread we haven't seen before. Should only happen at trace start.
    // Create a two-item stack of just user-mode pid and sched_syscall
    BrandNewPid(newpid, name, perpidstate);
  }

  thiscpu->cpu_stack = (*perpidstate)[newpid];

if (verbose) {
 fprintf(stdout, "new %d: ", newpid);
 DumpStackShort(stdout, &thiscpu->cpu_stack);
 fprintf(stdout, "\n");
 }
}

// An ambiguous call stack might be running in the current top or might be
// running in user mode. We look at the terminating event of the current 
// CPU span to try to resolve which it is.
void FixupAmbiguousSpan(const OneSpan& event, 
                      CPUState* thiscpu) {
  if (thiscpu->cpu_stack.ambiguous == 0) {return;}
  // If running above the ambiguous stack entry, nothing to do
  if (thiscpu->cpu_stack.ambiguous < thiscpu->cpu_stack.top) {return;}

if (verbose) {
DumpStackShort(stdout, &thiscpu->cpu_stack);
fprintf(stdout, " ===ambiguous at %s :\n", event.name.c_str());
}
  if (OnlyInKernelMode(event)) {
    thiscpu->cpu_stack.ambiguous = 0;
    // Span was set to top of stack, so we are all done
if (verbose) fprintf(stdout, "=== resolved kernel\n");
    return;
  }
  if (OnlyInUserMode(event)) {
    thiscpu->cpu_stack.ambiguous = 0;
    // Span was set to top of stack, but we need to pop back to user mode
    thiscpu->cpu_stack.top = 0;
    thiscpu->cur_span.eventnum = thiscpu->cpu_stack.eventnum[0];
    thiscpu->cur_span.name = thiscpu->cpu_stack.name[0];
if (verbose) fprintf(stdout, "=== resolved user\n");
    return;
  }
  // If neither, leave ambiguous. Span shows top of stack
if (verbose) fprintf(stdout, "=== unresolved\n");
}

uint64 PackLock(int lockhash, int pid) {
  uint64 retval = pid & 0x00000000ffffffffllu; 
  retval |= (lockhash &  0x00000000ffffffffllu) << 32;
  return retval;
}

void WriteFreqSpan(uint64 start_ts, uint64 end_ts, uint64 cpu, uint64 freq) {
  OneSpan event;
  event.start_ts = start_ts;
  event.duration = end_ts - start_ts;
  event.cpu = cpu;
  event.pid = 0;
  event.rpcid = 0;
  event.eventnum = KUTRACE_PSTATE;
  event.arg = freq;
  event.retval = 0;
  event.ipc = 0;
  event.name = string("freq");
  WriteEventJson(stdout, &event);
}

      
//
// Each call will update the current duration for this CPU and emit it, except
//  A few events do not close the current span:
//    PC samples, pstate changes (freq), point events not below
//  A few events interrupt the current span but then leave it continuing with new ts_start:
//    RPC events, mark_a/b/c/d, c_exit, mwait
//  
//
void ProcessEvent(const OneSpan& event, 
                  CPUState* cpustate, 
                  PerPidState* perpidstate) {
  CPUState* thiscpu = &cpustate[event.cpu];

  if (verbose) {
    fprintf(stdout, "zz[%d] %llu %llu %03x(%d)=%d %s ", 
          event.cpu, event.start_ts, event.duration, 
          event.eventnum, event.arg, event.retval, event.name.c_str());
    DumpEvent(stdout, "", event);
    DumpShort(stdout, &cpustate[event.cpu]);
  }

  // Remember last instance of each PID
  // We want to do this for the events that finish execution spans
  if ((event.pid > 0) && (event.cpu >= 0)) {
    priorPidEvent[event.pid] = event;
//fprintf(stdout, "~~ ~~ priorPidEvent[%d] = %llu\n", event.pid, event.start_ts);
  }

  // Remember that there is no pending context switch
  if (IsSchedCallEvent(event) || IsSchedReturnEvent(event)) {
    thiscpu->ctx_switch_ts = 0;
//fprintf(stdout, "~~ ~~ ctx_switch_ts[%d] = 0\n", event.cpu);
  }

  // Keep track of which PIDs are currently running
  if (IsSchedReturnEvent(event)) {
    pidRunning.erase(thiscpu->oldpid);
    pidRunning[thiscpu->newpid] = true;

    // Restore nonzero rpcid for a preempted task that we are returning to
    if (thiscpu->cpu_stack.rpcid != 0) {
      OneSpan temp_span;
      MakeRpcidMidSpan(event.start_ts, event.cpu, event.pid, thiscpu->cpu_stack.rpcid, &temp_span);
      WriteSpanJson2(stdout, &temp_span);
    }
  }

  // This event may reveal whether the current span is executing in user or kernel mode
  FixupAmbiguousSpan(event, thiscpu);

  // Normally, rawtoevent propogates the RPCID from these events to subsequent events.
  // Unfortunately, this doesn't track across context switches when an active RPC is
  // preempted and then later resumed.
  // So instead, we completely reconstruct rpcid here in eventtospan3, usin gnot event.rpcid
  // but thiscpu->rpcid, which is saved and restored across context switches.
  // An RPC event sets thiscpu->rpcid, and thiscpu->rpcid overrides event.rpc otherwise
  
  if (IsAnRpc(event)) {
    // Start a new span here so we don't assign RPC time to previous work
    if (thiscpu->valid_span) {
      // Prior span stops here 					--------^^^^^^^^
      FinishSpan(event, &thiscpu->cur_span);
      WriteSpanJson(stdout, thiscpu);	// Previous span
    }
    WriteEventJson(stdout, &event);	// Standalone mark 
    
// This is looking just like IsAMark
// Just update the still-open span start
    // Continue what we were doing, with new start_ts
    thiscpu->cur_span.start_ts = event.start_ts + event.duration;
    // Update CPU and current span's rpcid
    thiscpu->cpu_stack.rpcid = event.arg;	// 2021.02.05
    thiscpu->cur_span.rpcid = event.arg;
    return;
  }


  if (IsAContextSwitch(event)) {
    // Context switch
    // Current user-mode pid, seen at context switch and at front of each 
    // trace block. 
    // We expect this to match the [0] entry of the cpu's thread stack,
    // but it might not at the very front of a trace or at the oldest blocks
    // of a wraparound trace. When that happens, overwrite stack[0].
    // If the stack top is 0, also update the current span. 

    // Remember this pending context switch time, in case /sched is missing
    thiscpu->ctx_switch_ts = event.start_ts;
//fprintf(stdout, "~~ ~~ ctx_switch_ts[%d] = %llu\n", event.cpu, event.start_ts);

    // Mark the old stack ambiguous if inside kernel code
    thiscpu->cpu_stack.ambiguous = 0;
if (verbose) DumpStackShort(stdout, &thiscpu->cpu_stack);
    if (2 <= thiscpu->cpu_stack.top) {
      // Scheduler entered from within a kernel routine
      // stack such as: 2{mystery25.3950 read -sched- }0
      // Record the subscript of the ambiguous stack entry just before -sched-
if (verbose) fprintf(stdout, " ===marking old stack ambiguous at ctx_switch to %s\n", event.name.c_str());
      thiscpu->cpu_stack.ambiguous = thiscpu->cpu_stack.top - 1;
    }

    thiscpu->oldpid = EventnumToPid(thiscpu->cpu_stack.eventnum[0]);
    thiscpu->newpid = event.pid;

    //
    // Swap out the old thread's stack and swap in the new thread's stack
    //
    SwapStacks(thiscpu->oldpid, thiscpu->newpid, event.name, thiscpu, perpidstate);

    // We came in with the sched span started. just leave it alone here. 

#if 1
    // Turn context switch event into a user-mode-execution event at top of stack
    thiscpu->cpu_stack.eventnum[0] = PidToEventnum(event.pid);
    ////sthiscpu->cpu_stack.name[0] = EventNamePlusPid(event);
    thiscpu->cpu_stack.name[0] = NameAppendPid(pidnames[event.pid], event.pid);

    // And also update the current span if we are at top
    if (thiscpu->cpu_stack.top == 0) {
      // Update user-mode-execution event at top of stack
      //VERYTEMP
      bool xx = false && (thiscpu->cur_span.eventnum != PidToEventnum(event.pid));
      if (xx) {
        fprintf(stderr, "oldevent=%05x newevent=%05x\n", thiscpu->cur_span.eventnum, PidToEventnum(event.pid));
      }
      StartSpan(event, &thiscpu->cur_span);  // userexec span start 	--------vvvvvvvv
      thiscpu->valid_span = true;
      thiscpu->cur_span.eventnum = thiscpu->cpu_stack.eventnum[thiscpu->cpu_stack.top]; 
      thiscpu->cur_span.name = thiscpu->cpu_stack.name[thiscpu->cpu_stack.top]; 

      if (xx) {
        DumpEvent(stderr, "ctx", event);
        DumpSpan(stderr, " ctx", &thiscpu->cur_span);
        DumpStack(stderr, "ctx", &thiscpu->cpu_stack);
      }
    }
#endif

    return;
  }

  // If we have a PC sample for this CPU, assign it a duration up to the following sample
  // We do this by buffering one sample and emitting it later
  // Do not touch current span
  if (IsAPcSample(event)) {
    // Sample goes back to prior sample, if any
    if (thiscpu->prior_pc_samp_ts != 0) {
      // event is const so we can't modify it
      OneSpan event1 = event;
      event1.start_ts = thiscpu->prior_pc_samp_ts;
      event1.duration = event.start_ts - event1.start_ts;
      WriteEventJson(stdout, &event1);
    }
    thiscpu->prior_pc_samp_ts = event.start_ts;
    return;
  }

  // Similar for pstate (clock speed)
  // Do not touch current span
  if (IsAPstate(event)) {
    // PSTATE sampled freq  goes back to prior pstate, if any
    // PSTATE2 notified freq goes forward to next pstate2, if any
    // PSTATE: prior_pstate_ts..now = the current freq
    // PSTATE2: prior_pstate_ts..now = the prior frequency
    // At end of trace, we will flush out the last span

    if (thiscpu->prior_pstate_ts != 0) {
      uint64 prior_ts = thiscpu->prior_pstate_ts;
      uint64 this_freq = event.arg;
      uint64 prior_freq = thiscpu->prior_pstate_freq;
      uint64 freq = (event.eventnum == KUTRACE_PSTATE) ? this_freq : prior_freq;
      if (is_rpi) {
        // Reflect frequency on all CPUs
        for (int cpu = 0; cpu <= max_cpu_seen; ++cpu) {
          WriteFreqSpan(prior_ts, event.start_ts, cpu, freq);
       }
      } else {
        WriteFreqSpan(prior_ts, event.start_ts, event.cpu, freq);
      }
    }

    // Update one or all CPUs
    if (is_rpi) {
      // Change all CPU frequencies
      for (int cpu = 0; cpu <= max_cpu_seen; ++cpu) {
        cpustate[cpu].prior_pstate_ts = event.start_ts;
        cpustate[cpu].prior_pstate_freq = event.arg;
      }
    } else {
      thiscpu->prior_pstate_ts = event.start_ts;
      thiscpu->prior_pstate_freq = event.arg;
    }

   return;
  }

  // If we have a non-KUTRACE_USERPID point event, do not affect the current span. 
  // Just write the point event now, leaving the current span open to be
  // completed at a subsequent event
  //
  // 2019.05.14. Except go ahead and break spans at marks and mwait
  if (IsAMark(event) || IsAnMwait(event)) {
    if (thiscpu->valid_span) {
      // Prior span stops here 					--------^^^^^^^^
      FinishSpan(event, &thiscpu->cur_span);
      WriteSpanJson(stdout, thiscpu);	// Previous span
    }
    WriteEventJson(stdout, &event);	// Standalone mark/mwait/etc. 
    // Continue what we were doing, with new start_ts
    thiscpu->cur_span.start_ts = event.start_ts + event.duration;

    // Remember any mwait by cpu, for drawing c-state exit sine wave
    if (IsAnMwait(event)) {
      thiscpu->mwait_pending = event.arg;
      thiscpu->cur_span.arg = 1;	// Mark continuing idle as low-power
      thiscpu->cur_span.name = kIdlelpName;
    }

    return; 

  // Do not touch current span
  //     userpid, rpc, runnable, ipi, [mwait], pstate, [mark], lock, pc, wait
  } else if (IsAPointEvent(event)) {	// Marks do not end up here due to test just above
    WriteEventJson(stdout, &event);	// Standalone point event  
//VERYTEMP
//if (IsAnRpcMsg(event)) {DumpEvent(stderr, "rpcmsg:", event);}

/*****
Drawing lock-held lines
Normal long case: process A fails to get a lock, spins/waits until process B frees the lock and wakes up A, A exits wait and tries again
	Draw B lock_line from A fail to B free
Normal short case: process A fails to get a lock, spins until process B frees the lock BUT no wakeup, A exits spin and tries again
	Draw A lock_dots from A fail to A fail/acq (we don't know which process held the lock, which appears uncontended to B)
Abnormal case: Old file has no process B KUTRACE_LOCKWAKEUP but has B set-runnable A arc and A is waiting on a lock. 
	Draw lock_line from A fail to B beginning of futex that contains set-runnable
Multiple waiters: process A' fails to get lock with A already waiting. Do not change start ts

Prior		Event			Action
none		LOCKNOACQUIRE		set LockContend{ts-1us, -1}	unknown lock holder. silent acquire
LOCKNOACQUIRE	LOCKNOACQUIRE		 --				normal. multiple tries
LOCKACQUIRE	LOCKNOACQUIRE		 --				normal. acq pid holds lock
LOCKWAKEUP	LOCKNOACQUIRE		BUG

none		LOCKACQUIRE		set LockContend{ts, pid}		normal. continuing waiters
LOCKNOACQUIRE	LOCKACQUIRE		line/dots-1us, set LockContend{ts, pid}	silent release
LOCKACQUIRE	LOCKACQUIRE		line/dots-1us, set LockContend{ts, pid}	silent release
LOCKWAKEUP	LOCKACQUIRE		BUG

none		LOCKWAKEUP		line before			silent acquire, sometime before by us
LOCKNOACQUIRE	LOCKWAKEUP		line, clear LockContend		normal.
LOCKACQUIRE	LOCKWAKEUP		line, clear LockContend		normal.
LOCKWAKEUP	LOCKWAKEUP		BUG

arg0 = lock hash
process A contended
at KUTRACE_LOCKNOACQUIRE, if hash unseen, remember ts,A,hash. Start of known lock held but by unknown PID holding it

at KUTRACE_LOCKNOACQUIRE, if hash already pending by our thread, there was a hidden wakeup by unknown PID
                          emit lock_dots from remembered ts to current ts - 1us, A, hash
                          reset prior ts to current ts for new contended span 
process A' contended
at KUTRACE_LOCKNOACQUIRE, if hash already pending by other thread fail, add prior start_ts,A',hash 

Process A contended acquire
at KUTRACE_LOCKACQUIRE,   if hash already noacq pending by our thread, there was a hidden wakeup by unknown PID
                          emit lock_dots from remembered ts to current ts - 1us, A, hash
                          remove current pid from waiters
  remember the acquire time -- it *is* contended or we would not have emitted an acquire
process B contended release
at KUTRACE_LOCKWAKEUP,    emit lock_line from start_ts - 1us to current ts, B, hash
                          remove all waiters for hash

releasing process holds the lock back to earliest of acquire or noacquire. If notne of those, nominal 1us
acquiring process may encounter a hidden wakeup after its own fail, in which case emit dots


// Contended lockpending 
// Really just map<lockhash, struct> where struct has ts and list of waiting pids
*****/

// Things that can happen in the trace
// 1) CPU A releases lock, spinning CPU B acquires it immediately, produces ACQ trace entry, then A produces REL entry 10-20ns later
// 2) CPU A releases lock, spinning CPU B acquires it immediately, produces ACQ trace entry, A produces REL entry with equal time
// 3) CPU A releases lock, just as spinning CPU B fails to acquire it, A produces REL entry, B then produces TRY entry 30ns later
//
// In these cases, we actually want to process the acq after the rel
// In the meantime, suppressing lock_held lines < 1/2 us helps
//
// 4) CPU A releases lock, spinning CPU B acquires it immediately, but interrupt delays recording ACQ entry, 
//    meanwhile CPU C fails to acquire with unknown holder of the lock
//


    // Point event
    // Remember any failed lock acquire event if nothing is pending
    if (event.eventnum == KUTRACE_LOCKNOACQUIRE) {
      // Remember that this PID is trying to get this lock
      int lockhash = event.arg;
      uint64 subscr = PackLock(lockhash, event.pid);
      LockContend lockcontend;
      lockcontend.start_ts = event.start_ts;
      lockcontend.pid = event.pid;
      lockcontend.eventnum = event.eventnum;
      lockpending[subscr] = lockcontend;
    }

    // Process any successful lock acquire event
    if (event.eventnum == KUTRACE_LOCKACQUIRE) {
      int lockhash = event.arg;
      uint64 subscr = PackLock(lockhash, event.pid);
      // If prior try, draw dots for this PID trying to get this lock
      if ((lockpending.find(subscr) != lockpending.end()) && 
          (lockpending[subscr].eventnum == KUTRACE_LOCKNOACQUIRE)) {
        uint64 start_ts = lockpending[subscr].start_ts;
        uint64 end_ts = event.start_ts - 1;	// Stop 10 ns early
        // Ignore contention < 250ns
        if  (25 <= (end_ts - start_ts)) {
          bool dots = true;
          string lockname = "~" + event.name.substr(4);	// Remove try_ acq_ rel_
          OneSpan temp_span;
          MakeLockSpan(dots, start_ts, end_ts, event.pid, 
                       lockhash, lockname, &temp_span);
          WriteSpanJson2(stdout, &temp_span);
        }
      }
      // Remember that this PID now holds this lock
      LockContend lockcontend;
      lockcontend.start_ts = event.start_ts;
      lockcontend.pid = event.pid;
      lockcontend.eventnum = event.eventnum ;
      lockpending[subscr] = lockcontend;
    }

    // Process any lock wakeup (release) event
    if (event.eventnum == KUTRACE_LOCKWAKEUP) {
      int lockhash = event.arg;
      uint64 subscr = PackLock(lockhash, event.pid);
      // If prior acq, draw line for this PID holding this lock
      if ((lockpending.find(subscr) != lockpending.end()) &&
          (lockpending[subscr].eventnum == KUTRACE_LOCKACQUIRE)) {
        uint64 start_ts = lockpending[subscr].start_ts;
        uint64 end_ts = event.start_ts - 1;	// Stop 10 ns early
        // Ignore contention < 250ns
        if (25 <= (end_ts - start_ts)) {
          bool dots = false;
          string lockname = "=" + event.name.substr(4);	// Remove try_ acq_ rel_
          OneSpan temp_span;
          MakeLockSpan(dots, start_ts, end_ts, event.pid, 
                       lockhash, lockname, &temp_span);
          WriteSpanJson2(stdout, &temp_span);
        }
      }
      // This PID is no longer interested in the lock
      lockpending.erase(subscr);
    }



    // Point event
    // Remember any make-runnable, aka wakeup, event by target pid, for drawing arc
    if (IsAWakeup(event)) {
      WaitBeforeWakeup(event, cpustate, perpidstate);
      DoWakeup(event, cpustate, perpidstate);
      WaitAfterWakeup(event, cpustate, perpidstate);
    }

    return;
  }	// End point event

  OneSpan oldspan = thiscpu->cur_span;
  // +-----+-----+-----+-----+-----+-----+-----+-----+-----+-----------+
  // | ts  | /// | cpu | pid | rpc |event| arg | ret | /// |  name     |
  // +-----+-----+-----+-----+-----+-----+-----+-----+-----+-----------+

  // Prior span stops here 					--------^^^^^^^^
  if (thiscpu->valid_span) {
    FinishSpan(event, &thiscpu->cur_span);
    // Suppress idle spans of length zero or exactly 10ns
    bool suppress = ((thiscpu->cur_span.duration <= 1) && IsAnIdlenum(thiscpu->cur_span.eventnum));
    if (!suppress) {WriteSpanJson(stdout, thiscpu);}	// Previous span
  }

  // Connect wakeup event to new span if the PID matches
  if (pendingWakeup.find(event.pid) != pendingWakeup.end()) {
    // We are at an event w/pid for which there is a pending wakeup, make-runnable
    // Make a wakeup arc
    OneSpan temp_span = thiscpu->cur_span;	// Save
    MakeArcSpan(pendingWakeup[event.pid], event, &thiscpu->cur_span);
    WriteSpanJson(stdout, thiscpu);	// Standalone arc span
    // Consume the pending wakeup
    pendingWakeup.erase(event.pid);
    thiscpu->cur_span = temp_span;		// Restore
  }

  // Make a wait_cpu display span from the wakeup to here
  if (priorPidEnd.find(event.pid) != priorPidEnd.end()) {
    // We have been waiting for a CPU to become available and it did.
    OneSpan temp_span = thiscpu->cur_span;	// Save
    MakeWaitSpan('c', priorPidEnd[event.pid], event.start_ts, event.pid, 0, &thiscpu->cur_span);

    ////// Consume the pending wait
    ////priorPidEnd.erase(event.pid);
    priorPidEnd[event.pid] = event.start_ts + event.duration;
    // Don't clutter if the waiting is short (say < 10 usec)
    if (thiscpu->cur_span.duration >= kMIN_WAIT_DURATION) {
      WriteSpanJson(stdout, thiscpu);	// Standalone wait_cpu span
    }
    thiscpu->cur_span = temp_span;		// Restore
  }

  // Don't start new span quite yet.
  // If we have a return from foo and foo is on the stack, all is good.
  // But if we have a return from foo and there is no foo on the stack, we don't
  // want to overwrite whatever is there until we push a fake foo
  
  // Optimized calls are both call/return and are treated here as call
  if (IsACall(event)) {
    StartSpan(event, &thiscpu->cur_span);  // Call span start 	--------vvvvvvvv
    thiscpu->valid_span = true;

    if (IsOptimizedCall(event)) {
      AdjustStackForPush(event, thiscpu);	// Die if any -- preproc failed
      // Emit the call span now but don't push
      thiscpu->cur_span.duration = event.duration;
      // Note: Optimized call/ret, prior span ipc in ipc<3:0>, current span in ipc<7:4>
      thiscpu->cur_span.ipc = (event.ipc >> 4) & ipc_mask;
      WriteSpanJson(stdout, thiscpu);	// Standalone call-return span
      // Continue what we were doing, with new start_ts
      thiscpu->cur_span = oldspan;
      thiscpu->cur_span.start_ts = event.start_ts + event.duration;
    } else {
      // Non-optimized call
      // Push newly-pending call for later matching return
      AdjustStackForPush(event, thiscpu);	// Die if any -- preproc failed
      ++thiscpu->cpu_stack.top;
      thiscpu->cpu_stack.eventnum[thiscpu->cpu_stack.top] = event.eventnum;
      thiscpu->cpu_stack.name[thiscpu->cpu_stack.top] = event.name;
    }

  } else if (IsAReturn(event)) {    
    // Called span we are returning from got closed above. Start just after return.
    // Adjust first, then start span at proper nesting level
    AdjustStackForPop(event, thiscpu);	// Die if any -- preproc failed
    --thiscpu->cpu_stack.top;
    StartSpan(event, &thiscpu->cur_span);  // Post-return span 	--------vvvvvvvv
    thiscpu->valid_span = true;
    // If ambiguous, this defaults to the top of stack, e.g. inside a syscall
    // When we finish this span, we will try to resolve the ambiguity and possibly change
    // eventnum and name to the usermode PID at cpu_stack[0]
    thiscpu->cur_span.eventnum = thiscpu->cpu_stack.eventnum[thiscpu->cpu_stack.top]; 
    thiscpu->cur_span.name = thiscpu->cpu_stack.name[thiscpu->cpu_stack.top]; 

  } else if (IsUserExec(event)) {		// context switch
#if 1
    StartSpan(event, &thiscpu->cur_span);  // Post-switch span 	--------vvvvvvvv
    thiscpu->valid_span = true;
#endif

  } else {
    // c-exit and other synthesized items
    // Make it a standalone span and go back to what was running
    WriteEventJson(stdout, &event);  
    // Continue what we were doing, with new start_ts
    StartSpan(event, &thiscpu->cur_span);  // New start 	--------vvvvvvvv
    thiscpu->valid_span = true;
    thiscpu->cur_span = oldspan;
    thiscpu->cur_span.start_ts = event.start_ts + event.duration;
  }
}	// End ProcessEvent


// Process one inserted event
void InsertEvent(const OneSpan& event, 
                 CPUState* cpustate, 
                 PerPidState* perpidstate) {
  if (verbose) {
    DumpEvent(stdout, "insert:", event);
  }
  ProcessEvent(event, cpustate, perpidstate);
}



int CallToRet(int eventnum) {return eventnum | ret_mask;}
int RetToCall(int eventnum) {return eventnum & ~ret_mask;}

string CallnameToRetname(string name) {return "/" + name;}	// Add '/'
string RetnameToCallname(string name) {return name.substr(1);}  // Remove '/'

// Insert a dummy return at ts from TOS
void InsertReturnAt(uint64 ts, 
                 const OneSpan& event, 
                 CPUState* cpustate, 
                 PerPidState* perpidstate) {
  CPUState* thiscpu = &cpustate[event.cpu];
  PidState* thiscpu_stack = &thiscpu->cpu_stack;

  OneSpan newevent = event;
  newevent.start_ts = ts;	
  newevent.duration = 0;
  //newevent.cpu = xx;
  //newevent.pid = xx;
  //newevent.rpcid = xx;
  newevent.eventnum = CallToRet(thiscpu_stack->eventnum[thiscpu_stack->top]);
  newevent.arg = 0;
  newevent.retval = 0;
  //newevent.ipc = 0;
  newevent.name = CallnameToRetname(thiscpu_stack->name[thiscpu_stack->top]);
  InsertEvent(newevent, cpustate, perpidstate);
}

// Insert a dummy call at ts to event (which is a return)
void InsertCallAt(uint64 ts, 
                 const OneSpan& event, 
                 CPUState* cpustate, 
                 PerPidState* perpidstate) {
  OneSpan newevent = event;
  newevent.start_ts = ts;	
  newevent.duration = 0;
  //newevent.cpu = xx;
  //newevent.pid = xx;
  //newevent.rpcid = xx;
  newevent.eventnum = RetToCall(event.eventnum);
  newevent.arg = 0;
  newevent.retval = 0;
  //newevent.ipc = 0;
  newevent.name = RetnameToCallname(event.name);
  InsertEvent(newevent, cpustate, perpidstate);
}

// Insert a dummy call/return at ts to event (which is a call)
void InsertCallRetAt(uint64 ts, 
                 const OneSpan& event, 
                 CPUState* cpustate, 
                 PerPidState* perpidstate) {
  OneSpan newevent = event;
  newevent.start_ts = ts;	
  //newevent.duration = xx;
  //newevent.cpu = xx;
  //newevent.pid = xx;
  //newevent.rpcid = xx;
  //newevent.eventnum = xx;
  //newevent.arg = xx;
  //newevent.retval = xx;
  //newevent.ipc = xx;
  //newevent.name = xx;
  InsertEvent(newevent, cpustate, perpidstate);
}

// Return from X
// if TOS = call to X, all is good
// if X is on the stack, pop to it
// if TOS = call to something that nests inside X, pop and repeat
// if TOS = Y and X is higher on the stack
// Fixup: If this event is a return from X and X has a smaller nesting level than top of stack,
// insert extra returns and pop stack
// Return true if the event is to be used, and false if it is to be discarded
bool FixupReturn(uint64 new_start_ts, 
                 const OneSpan& event, 
                 CPUState* cpustate, 
                 PerPidState* perpidstate) {
  CPUState* thiscpu = &cpustate[event.cpu];
  PidState* thiscpu_stack = &thiscpu->cpu_stack;
  int matching_callnum = RetToCall(event.eventnum);
  
  // if TOS = call to X, all is good
  if (thiscpu_stack->eventnum[thiscpu_stack->top] == matching_callnum) {return true;}

  // If TOS = reschedule_ipi and this = /BH:hi, let it match
  if ((thiscpu_stack->name[thiscpu_stack->top] == "reschedule_ipi") && 
      (event.name == "/BH:hi")) {return true;}

  bool callfound = false;
  for (int i = 1; i <= thiscpu_stack->top; ++i) {
    if (thiscpu_stack->eventnum[i] == matching_callnum) {callfound = true;}
  }

  // if X is on the stack, pop to it
  OneSpan newevent;
  if (callfound) {
    // Insert dummy returns at now until TOS = X (we don't know the retval)
    while (thiscpu_stack->eventnum[thiscpu_stack->top] != matching_callnum) {
      // Return now from TOS
if(verbose){fprintf(stdout, "InsertReturnAt 1\n");}
      InsertReturnAt(event.start_ts, event, cpustate, perpidstate);
    }
    return true;
  }

  // At start of current span, insert dummy returns until nesting X is OK, then a dummy call to X

  // Insert dummy returns at new_start_ts until nesting X is OK (we don't know the retval)
  while (NestLevel(matching_callnum) <= NestLevel(thiscpu_stack->eventnum[thiscpu_stack->top])) {
    // Return at span_start_ts from TOS
if(verbose){fprintf(stdout, "InsertReturnAt 2\n");}
    InsertReturnAt(new_start_ts, event, cpustate, perpidstate);
  }

  // Insert dummy call at span_start_ts to X (we don't know the arg value)
  InsertCallAt(new_start_ts, event, cpustate, perpidstate);
  return true;
}

bool FixupCall(uint64 new_start_ts, 
                 const OneSpan& event, 
                 CPUState* cpustate, 
                 PerPidState* perpidstate) {
  CPUState* thiscpu = &cpustate[event.cpu];
  PidState* thiscpu_stack = &thiscpu->cpu_stack;
  int matching_callnum = RetToCall(event.eventnum);

  // Insert dummy returns at new_start_ts until nesting X is OK (we don't know the retval)
  while (NestLevel(matching_callnum) <= NestLevel(thiscpu_stack->eventnum[thiscpu_stack->top])) {
    // Return at span_start_ts from TOS
if(verbose){fprintf(stdout, "InsertReturnAt 3: %d %d\n", matching_callnum, thiscpu_stack->eventnum[thiscpu_stack->top]);}
    InsertReturnAt(new_start_ts, event, cpustate, perpidstate);
  }
  return true;
}

bool FixupResched(uint64 ts, 
                 const OneSpan& event, 
                 CPUState* cpustate, 
                 PerPidState* perpidstate) {
  CPUState* thiscpu = &cpustate[event.cpu];
  PidState* thiscpu_stack = &thiscpu->cpu_stack;
  if (thiscpu_stack->name[thiscpu_stack->top] == "reschedule_ipi") {
    --thiscpu_stack->top;
  }
  return true;
}


// We are exactly in sched but missing its return
bool FixupSched(uint64 new_start_ts, 
                 const OneSpan& event, 
                 CPUState* cpustate, 
                 PerPidState* perpidstate) {
  CPUState* thiscpu = &cpustate[event.cpu];
  PidState* thiscpu_stack = &thiscpu->cpu_stack;
  int matching_callnum = RetToCall(event.eventnum);

  // Return at new_start_ts from TOS
if(verbose){fprintf(stdout, "InsertReturnAt 4\n");}
  InsertReturnAt(new_start_ts, event, cpustate, perpidstate);
  ////--thiscpu_stack->top;
  return true;
}


// Fixup: Turn idle/mwait/idle/X into idle/mwait/idle/c-exit/X
//   We are at X
// idle is on the stack[0] as PID=0 and as current span
bool FixupCexit(uint64 new_start_ts, 
                 const OneSpan& event, 
                 CPUState* cpustate, 
                 PerPidState* perpidstate) {
  CPUState* thiscpu = &cpustate[event.cpu];
  PidState* thiscpu_stack = &thiscpu->cpu_stack;

  // Table entries are unknown units; they appear to be multiples of 100ns
  uint64 exit_latency = kLatencyTable[thiscpu->mwait_pending] * 10;

  uint64 pending_span_latency = new_start_ts - thiscpu->cur_span.start_ts;

  bool good_mwait = (thiscpu->cpu_stack.top == 0); 	// Expecting to be in user-mode
  if (!good_mwait) {
    // No change -- we are not immediately after a switch to idle 
    fprintf(stderr, "FixupCexit ignored %llu %llu %llu %d %05x\n", 
            new_start_ts, exit_latency, pending_span_latency,
            thiscpu->cpu_stack.top, thiscpu->cpu_stack.eventnum[0]);
    return true;
  }

  // Calculate exit_latency = min(exit_latency, pending_span_latency)
  if (pending_span_latency < exit_latency) {
    // Actual remaining idle is shorter than supposed exit latency.
    exit_latency = pending_span_latency;  
  } 
  // If too short, don't bother with the c-exit
  if (exit_latency < kMIN_CEXIT_DURATION) {return true;}

  // Inserting the c-exit will shorten the pending idle
  ////thiscpu->cur_span.duration -= exit_latency;
////fprintf(stdout, "~~duration[%d] -= %llu = %llu\n", event.cpu, exit_latency, thiscpu->cur_span.duration);

  // Insert c-exit call/ret
  uint64 cexit_start_ts = new_start_ts - exit_latency;
  OneSpan newevent = event;
  newevent.start_ts = cexit_start_ts;	
  newevent.duration = exit_latency;
  //newevent.cpu = xx;
  //newevent.pid = xx;
  //newevent.rpcid = xx;
  newevent.eventnum = event_c_exit;	// Treated as a call/ret
  newevent.arg = 0;
  newevent.retval = 0;
  newevent.ipc = 0;
  newevent.name = "-c-exit-";
  // Inserting the c-exit shortens the pending low-power idle
  InsertEvent(newevent, cpustate, perpidstate);	

  // After the c-exit, we are no longer low power
  thiscpu->cur_span.arg = 0;	// Mark continuing idle as normal power
  thiscpu->cur_span.name = kIdleName;

  return true;
}      


// Insert a make-runnable event at  clone, fork, etc.
bool FixupRunnable(uint64 new_start_ts, 
                 const OneSpan& event, 
                 CPUState* cpustate, 
                 PerPidState* perpidstate) {
  // We can be called with standalone call event, optimized call/ret, or ret
  // If standalone call, we don't know the end time yet, so just return.
  if (IsACall(event) && !IsOptimizedCall(event)) {return true;}

  // Insert runnable
  OneSpan newevent = event;
  newevent.start_ts = new_start_ts;	
  newevent.duration = 1;
  //newevent.cpu = xx;
  //newevent.pid = xx;
  //newevent.rpcid = xx;
  newevent.eventnum = KUTRACE_RUNNABLE;
  newevent.arg = event.retval;	// The target of clone/fork/etc.
  newevent.retval = 0;
  newevent.ipc = 0;
  newevent.name = "runnable";
  InsertEvent(newevent, cpustate, perpidstate);
  return true;
}

// Insert an RPC msg event, describing msg span of packets on the network

bool EmitRxTxMsg(const PidCorr& corr, CPUState* cpustate, PerPidState* perpidstate) {
//fprintf(stderr, "EmitRxTxMsg ts/rpcid/lglen8/rx = %llu %u %u %u\n", corr.k_timestamp, corr.rpcid, corr.lglen8, corr.rx);
  uint64 k_timestamp;	// Time kernel code saw hash32. 0 means not known yet
  uint32 rpcid;		// 0 means not known yet
  uint16 lglen8;	// 0 means not known yet
  bool rx;		// true if rx 

  if ((corr.k_timestamp == 0) || (corr.rpcid == 0) || (corr.lglen8 == 0)) {
    return true;
  }
  char msg_name[64];
  sprintf(msg_name, "%s.%d", methodnames[corr.rpcid].c_str(), corr.rpcid);
  uint64 msg_len = TenPow(corr.lglen8);
  uint64 dur = msg_dur_10nsec(msg_len);	// Increments of 10ns
  uint64 msg_event = corr.rx ? KUTRACE_RPCIDRXMSG : KUTRACE_RPCIDTXMSG;

  // Insert RpcMsg
  OneSpan newevent;
  // Subtracting duration shows incoming packets ending at kernel timestamp
  // Outgoing packets start at kernel timestamp
  newevent.start_ts = corr.k_timestamp - (corr.rx ? dur : 0LLU);	
  newevent.duration = dur;
  newevent.cpu = 0;
  newevent.pid = 0;
  newevent.rpcid = corr.rpcid;
  newevent.eventnum = msg_event;
  newevent.arg = msg_len;
  newevent.retval = 0;
  newevent.ipc = 0;
  newevent.name = string(msg_name);
//DumpEvent(stderr, "EmitRxTxMsg:", newevent);
  InsertEvent(newevent, cpustate, perpidstate);
  return true;
}

// If the length is nearly 0, this is from an old client4/server4 that did not include 
// the signature/header size.
// Set it here to TenLg(16 + 72) = lg(88) * 10 = 6.46, so 64
uint64 FixupLength(uint64 lglen8) {
  return uint64max(64, lglen8);
}


//---------------------------------------------------------------------------//
// Preprocess cleans up the input events:
//   - Insert any missing calls/ returns
//   - Insert any missing make-runnable
//   - Insert mwait sine waves
//   - Insert any missing names
//   - Insert wait_* events
//   - Insert wakeup-to-exec arcs
//
void PreProcessEvent(const OneSpan& event, 
                     CPUState* cpustate, 
                     PerPidState* perpidstate) {
  CPUState* thiscpu = &cpustate[event.cpu];
  PidState* thiscpu_stack = &thiscpu->cpu_stack;

  // Fixups may say to delete this event
  bool keep = true;

  // The start_ts to insert fixups can be the current time, event.start_ts,
  // or the start of the current span, thiscpu->cur_span.start_ts, but only
  // if the current span is valid
  uint64 span_start_time = thiscpu->cur_span.start_ts;
  if (!thiscpu->valid_span) {span_start_time = event.start_ts;}

  //--------------------------------------------//
  // Things to insert BEFORE the current event  //
  //--------------------------------------------//

  //   - Insert any missing calls/ returns
  // Fixup: return from X
  // if TOS = call to X, all is good, else pop as needed
  // If X is not on the stack, insert call to it at span_start_ts
  if (IsAReturn(event)) { 
    keep &= FixupReturn(span_start_time, event, cpustate, perpidstate);
  }

  // Fixup: a call to -sched- with reschedule_ipi on the stack silently pops it off 
  if (IsSchedCallEvent(event)) {
    keep &= FixupResched(span_start_time, event, cpustate, perpidstate);
  }

  // Fixup: a syscall/irq/fault INSIDE -sched-; we missed the return from sched 
  // insert a return from sched at context switch if any,
  // putting us back at top-level user-mode 
  if ((thiscpu->ctx_switch_ts > 0) && IsACall(event) && (thiscpu_stack->top == 1) && 
      (IsSchedCallEventnum(thiscpu_stack->eventnum[thiscpu_stack->top]))) {
    keep &= FixupSched(thiscpu->ctx_switch_ts, event, cpustate, perpidstate);
  }

  // Fixup: call to X 
  // if TOS = call to something that must nest inside X, pop and repeat
  if (IsACall(event)) { 
    keep &= FixupCall(event.start_ts, event, cpustate, perpidstate);
  }

  // Insert mwait sine waves
  // The earlier mwait already changed idle/mwait/X to idle/mwait/idle/X
  // We are at X
  // Fixup: Turn idle/mwait/X into idle'/mwait/c-exit/X
  // events -- shorter idle followed by power C-state exit latency
  if (cpustate[event.cpu].mwait_pending > 0) {
    // We are at X
    keep &= FixupCexit(event.start_ts, event, cpustate, perpidstate);
    cpustate[event.cpu].mwait_pending = 0;
//fprintf(stdout, "~~mwait_pending[%d] = %d\n", event.cpu, 0);
  }

  //   - Insert any missing names
  //   - Insert wakeup-to-exec arc events

  //
  // Remember bits of state
  //

  // Remember last instance of each PID, for xxx
  // We want to do this for the events that finish execution spans
  if ((event.pid > 0) && (event.cpu >= 0)) {
    priorPidEvent[event.pid] = event;
//fprintf(stdout, "~~priorPidEvent[%d] = %llu\n", event.pid, event.start_ts);
  }

  // Remember that there is no pending context switch, for FixupSched
  if (IsSchedCallEvent(event) || IsSchedReturnEvent(event)) {
    thiscpu->ctx_switch_ts = 0;
//fprintf(stdout, "~~ctx_switch_ts[%d] = 0\n", event.cpu);
  }

  if (IsAContextSwitch(event)) {		// for FixupSched
    // Remember this pending context switch time, in case /sched is missing
    thiscpu->ctx_switch_ts = event.start_ts;
//fprintf(stdout, "~~ctx_switch_ts[%d] = %llu\n", event.cpu, event.start_ts);
  }

  // Remember any mwait by cpu, for drawing c-state exit sine wave
  if (IsAnMwait(event)) {			// For FixupCexit
    thiscpu->mwait_pending = event.arg;
//fprintf(stdout, "~~mwait_pending[%d] = %d\n", event.cpu, event.arg);
  }

  // Remember any failed lock acquire event, for wait_lock
  if (event.eventnum == KUTRACE_LOCKNOACQUIRE) {
    pendingLock[event.arg] = event;
    priorPidLock[event.pid] = event.arg;
//fprintf(stdout, "~~priorPidLock[%d] = %d\n", event.pid, event.arg);
  }

  // Enqueue/dequeue processing: make a queue span per RPC
  if (IsAnEnqueue(event)) {
    // Remember which queue the RPC is put on but wait until the
    // upcoming RPCIDREQ/RESP to use that timestamp
    // queue_num is in event.arg
    thiscpu->cpu_stack.enqueue_num_pending = event.arg; 
  }

  if (IsADequeue(event)) {
    // Remember which queue the RPC is removed from but wait until the
    // upcoming RPCIDREQ/RESP to use that timestamp
    thiscpu->cpu_stack.dequeue_num_pending = event.arg; 
  }

  // Go ahead and timestamp enq/deq either at RPC change or context switch
  if (IsAnRpc(event) || IsAContextSwitch(event)) {
    if (0 <= thiscpu->cpu_stack.enqueue_num_pending) {
      // Switching away from an RPC. Remember that queued span starts here
      // Old rpcid is in event.rpcid
      enqueuetime[event.rpcid] = event.start_ts + 1;	// Start used below
      thiscpu->cpu_stack.enqueue_num_pending = -1;
    }

    if (0 <= thiscpu->cpu_stack.dequeue_num_pending) {
      // Switching to new RPC. Emit a queued span ending here
      // New rpcid is in event.arg
      OneSpan temp_span;
      MakeQueuedSpan(enqueuetime[event.arg], event.start_ts - 1, 
                     thiscpu->cpu_stack.dequeue_num_pending, event.arg, &temp_span); 
      thiscpu->cpu_stack.dequeue_num_pending = -1;
      // Don't clutter if the queued waiting is short (say < 10 usec)
      if (temp_span.duration >= kMIN_WAIT_DURATION) {
        WriteSpanJson2(stdout, &temp_span);	// Standalone queued span
      }
    }
  }


//
// Begin RPC packet correlation
//
// NOTE: Must do incoming test before outgoing work
//
// Incoming event order
// RX_PKT:	remember kernal timestamp in rx_hashtocorr[hash32]
// RX_USER:	find k_ts in rx_hashtocorr[hash32], remember k_ts in pidtocorr[pid], 
//		erase rx_hashtocorr[hash32]
// RPCIDRE*:	have rpcid/length, find k_ts in pidtocorr[pid]; put out (rpcid/name/length/k_ts)
//		pidtocorr[pid]
  uint32 pkt_hash32 = (uint32)event.arg;

  if (IsRawRxPktInt(event.eventnum)) {
//DumpEvent(stderr, "IsRawRxPktInt:", event);
    rx_hashtocorr[pkt_hash32] = inithashcorr;
    rx_hashtocorr[pkt_hash32].k_timestamp = event.start_ts;
  }

  if (IsUserRxPktInt(event.eventnum)) {
//DumpEvent(stderr, "IsUserRxPktInt:", event);
    pidtocorr[event.pid] = initpidcorr;
    if (rx_hashtocorr.find(pkt_hash32) != rx_hashtocorr.end()) {
      pidtocorr[event.pid].k_timestamp = rx_hashtocorr[pkt_hash32].k_timestamp;
    }
    rx_hashtocorr.erase(pkt_hash32);
    pidtocorr[event.pid].rx = true;
  }

  if (IsIncomingRpcReqResp(event)) {
//DumpEvent(stderr, "IsIncomingRpcReqResp:", event);
    uint32 msg_rpcid16 = event.arg & 0xffff;
    uint16 msg_lglen8 = FixupLength((event.arg >> 16) & 0xff);
    pidtocorr[event.pid].rpcid = msg_rpcid16;
    pidtocorr[event.pid].lglen8 = msg_lglen8;
    keep &= EmitRxTxMsg(pidtocorr[event.pid], cpustate, perpidstate);
    pidtocorr.erase(event.pid);
  }

// Outgoing event order
// RPCIDRE*:	remember rpcid/length in pidtocorr[pid]
// TX_USER:	remember pid in tx_hashtocorr[hash32]
// TX_PKT:	have kernel timestamp, have pid in tx_hashtocorr[hash32], rpcid/length in pidtocorr[pid]; 
//		erase rx_hashtocorr[hash32]
//              put out (rpcid/name/length/k_ts)
//		erase pidtocorr[pid] 
  if (IsOutgoingRpcReqResp(event)) {
    // This creates a pidtocorr record. If the test for IsIncomingRpcReqResp
    // follows this, it will erroneously return true. So we do the
    // incoming correlation first, above.
//DumpEvent(stderr, "IsOutgoingRpcReqResp:", event);
    uint32 msg_rpcid16 = event.arg & 0xffff;
    uint16 msg_lglen8 = FixupLength((event.arg >> 16) & 0xff);
    pidtocorr[event.pid] = initpidcorr;
    pidtocorr[event.pid].rpcid = msg_rpcid16;
    pidtocorr[event.pid].lglen8 = msg_lglen8;
    pidtocorr[event.pid].rx = false;
  }

  if (IsUserTxPktInt(event.eventnum)) {
//DumpEvent(stderr, "IsUserTxPktInt:", event);
    tx_hashtocorr[pkt_hash32] = inithashcorr;
    tx_hashtocorr[pkt_hash32].pid = event.pid;
  }

  if (IsRawTxPktInt(event.eventnum)) {
//DumpEvent(stderr, "IsRawTxPktInt:", event);
    uint32 pid = 0;
    if (tx_hashtocorr.find(pkt_hash32) != tx_hashtocorr.end()) {
      pid = tx_hashtocorr[pkt_hash32].pid;
    }
    tx_hashtocorr.erase(pkt_hash32);
    if (pidtocorr.find(pid) != pidtocorr.end()) {
      pidtocorr[pid].k_timestamp = event.start_ts;
      keep &= EmitRxTxMsg(pidtocorr[pid], cpustate, perpidstate);
    }
    pidtocorr.erase(pid);
  }
// End RPC packet correlation


  if (event.eventnum == KUTRACE_MBIT_SEC) {
    mbit_sec = event.arg;
    keep = false;		// Not a JSON event -- moved to JSON metadata
  }

  //--------------------------------------------//
  // The current event                          //
  //--------------------------------------------//

  if (keep) {
    ProcessEvent(event, cpustate, perpidstate);
  }

  //--------------------------------------------//
  // Things to insert AFTER the current event   //
  //--------------------------------------------//

  //   - Insert any missing make-runnable
  // Do this AFTER the syscall/ret has been processed (keep is ignored)
  if (IsNewRunnablePidSyscall(event) && (event.retval != 0)) {
    keep &= FixupRunnable(event.start_ts + event.duration, event, cpustate, perpidstate);
  }

}	// End PreProcessEvent

//
//---------------------------------------------------------------------------//



// Fix PID names
// We do four things here:
// (1) For each PID number, keep its current name, in time order
// (2) For each PID number, keep a string of all the names assigned to it, in time order
//     first+second+third... for naming a row of the timeline display
// (3) Force idle PID name to be consistent
// (4) Update the name in each user-mode span

// We will see multiple names for a process ID after execve
// If we see a, then b, then c for the same PID, form a+b+c

// Record the current name for each process ID, PID
// If not the dummy names at time -1, also record any accumulated varying names for the same PID
void RecordPidName(int64 temp_ts, int temp_arg, char* temp_name, CPUState* cpustatep) {
  // temp_arg might be an event number, pid + 0x10000. If so, covert it to a PID number
  temp_arg = EventnumToPid(temp_arg);
  if (temp_arg == pid_idle) {return;}			// Never update idle name

  string temp_name_str = string(temp_name);
  // Turn "./kutrace_contro" into "kutrace_control"
  if (strcmp(temp_name, "./kutrace_contro") == 0) {
    temp_name_str = string("kutrace_control");
  }

  // Remove any leading ./ pathname
  if (memcmp(temp_name, "./", 2) == 0) {
    temp_name_str = string(temp_name + 2);
  }

  // Record current name for this pid
  pidnames[temp_arg] = temp_name_str;			// Current name for this PID
////fprintf(stderr, "pidname[%5d] = %s %lld\n", temp_arg, pidnames[temp_arg].c_str(), temp_ts);
		
  if (temp_ts == -1) {return;}

  // Record accumulated row name(s) for a PID
  if (pidrownames[temp_arg].find(temp_name_str) == string::npos) {
    // Add this pid to row name 
    if (pidrownames[temp_arg].empty()) {
      pidrownames[temp_arg] = temp_name_str;
    } else {
      pidrownames[temp_arg] = pidrownames[temp_arg] + "+" + temp_name_str;
////fprintf(stderr, "rowname[%5d] = %s\n", temp_arg, pidrownames[temp_arg].c_str());		
    }
  }

  // Update this name on any pending CPU stack
  for (int cpu = 0; cpu <= max_cpu_seen; ++cpu) {
    if(cpustatep[cpu].cpu_stack.eventnum[0] ==  PidToEventnum(temp_arg)) {
      cpustatep[cpu].cpu_stack.name[0] = NameAppendPid(temp_name_str, temp_arg);
    }
  }
}

void FixPidName(OneSpan* eventp) {
  if (!IsUserExec(*eventp) && !(IsAContextSwitch(*eventp))) {return;}

  // Force in the current name from pidnames[pid]
  int pid = EventnumToPid(eventp->eventnum);
  if (pidnames.find(pid) != pidnames.end()) {
    eventp->name = NameAppendPid(pidnames[pid], pid);
    // Also update the stacked name for this pid
    // also update the span name for this pid
    //stack->name[0]
  }
}

// For Raspberry PI, change mwait to wfi
void FixMwaitName(OneSpan* eventp) {
  if (is_rpi && IsAnMwait(*eventp)) {
    eventp->name = string("wfi");
  }

}

// This covers many naming sins
void FixNames(OneSpan* eventp) {
  FixPidName(eventp);
  // Fix lock name
  // Fix queue name
  // Fix RPC name
  // Fix misc. other names
  FixMwaitName(eventp);
}


static const int kMaxBufferSize = 256;

// Read next line, stripping any crlf. Return false if no more.
bool ReadLine(FILE* f, char* buffer, int maxsize) {
  char* s = fgets(buffer, maxsize, f);
  if (s == NULL) {return false;}
  int len = strlen(s);
  // Strip any crlf or cr or lf
  if (s[len - 1] == '\n') {s[--len] = '\0';}
  if (s[len - 1] == '\r') {s[--len] = '\0';}
  return true;
}

// We assign every nanosecond of each CPUs time to some time span.
// Initially, all CPUs are assumed to be executing the idle job, pid=0
// Any syscall/irq/trap pushes into that kernel code
// Any matching return pops back to the current user code
// Items can nest only in this order:
//  user ==> syscall ==> irq ==> trap
// In general, there will be missing and sometimes wrong information, so 
// this program needs to be robust in assigning time in meaningful ways.
// 
// We run a small stack for each CPU, and swap it away when there is a 
// context switch, bringing it back when there is a context switch back.
// 
// If we encounter a not-allowed transition, we insert pops and pushes as needed
// to make a correctly-nested set of time spans.

//
// Usage: eventtospan3 <event file name> [-v] [-t]
//
int main (int argc, const char** argv) {
  CPUState cpustate[kMAX_CPUS];	// Running state for each CPU
  PerPidState perpidstate;	// Saved PID call stacks, for context switching

  OneSpan event;
  string trace_label;
  string trace_timeofday;
  kernel_version.clear();
  cpu_model_name.clear();
  host_name.clear();
  methodnames.clear();
  pidtocorr.clear();
  rx_hashtocorr.clear();
  tx_hashtocorr.clear();


  if (argc >= 2) {
    // Pick off trace label from first argument, if any
    trace_label = string(argv[1]);
  }

  // Pick off other flags
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "-v") == 0) {verbose = true;}
    if (strcmp(argv[i], "-t") == 0) {trace = true;}
    if (strcmp(argv[i], "-rel0") == 0) {rel0 = true;}
  } 

  // Initialize CPU state
  for (int i = 0; i < kMAX_CPUS; ++i) {
    InitPidState(&cpustate[i].cpu_stack);
    InitSpan(&cpustate[i].cur_span, i);
    //InitSpan(&cpustate[i].prior_pc_sample, i);
    cpustate[i].prior_pstate_ts = 0;
    cpustate[i].prior_pstate_freq = 0;
    cpustate[i].prior_pc_samp_ts = 0;
    cpustate[i].ctx_switch_ts = 0;
    cpustate[i].mwait_pending = 0;
    cpustate[i].oldpid = 0;
    cpustate[i].newpid = 0;
    cpustate[i].valid_span = false;		// Ignore initial span  
  }

  // Set idle name
  pidnames[pid_idle] = string(kIdleName);
  pidrownames[pid_idle] = string(kIdleName);

  // PID 0, the idle task, is special. Multiple copies can be running on different CPUs, and
  // It can be in the midst of an interrupt when a context switch goes to another thread,
  // but the interrupt code is silently done.
  // Here we set the stacked idle task as inside sched, and we never change that elsewhere.
  BrandNewPid(pid_idle, string(kIdleName), &perpidstate);


  //
  // Main loop
  //
  uint64 lowest_ts = 0;
  uint64 prior_ts = 0;
  int linenum = 0;
  char buffer[kMaxBufferSize];
  while (ReadLine(stdin, buffer, kMaxBufferSize)) {
    ++linenum;
    int len = strlen(buffer);
    if (buffer[0] == '\0') {continue;}

    // Comments start with #, some are stylized and contain data
    if (buffer[0] == '#') {
      // Pull timestamp out of early comments
      // Look for first 
      // # [1] 2017-08-21_09:51:48.620665
      // Must be there. This triggers initial json output
      if ((len >= 32) && 
          trace_timeofday.empty() && 
          (memcmp(buffer, "# [1] 20", 8) == 0)) {
          // From # [1] 2019-03-16_16:43:42.571604
          // extract    2019-03-16_16:43:00
          // since the timestamps are all relative to a minute boundary
          trace_timeofday = string(buffer, 6, 17) + "00";
          //fprintf(stderr, "eventtospan3: trace_timeofday '%s'\n", trace_timeofday.c_str());
          InitialJson(stdout, trace_label.c_str(), trace_timeofday.c_str());
      }
      // Pull version and flags out if present
      if (memcmp(buffer, "# ## VERSION: ", 14) == 0) {
        incoming_version = atoi(buffer + 14); 
        //fprintf(stderr, "VERSION %d\n", incoming_version);
      }
      if (memcmp(buffer, "# ## FLAGS: ", 12) == 0) {
        incoming_flags = atoi(buffer + 12); 
        //fprintf(stderr, "FLAGS %d\n", incoming_flags);
      }
      continue;
    }

    // Input created by:
    //  fprintf(stdout, "%lld %lld %lld %lld  %lld %lld %lld %lld %d %s (%llx)\n", 
    //          mhz, duration, event, current_cpu, current_pid[current_cpu], current_rpc[current_cpu], 
    //          arg, retval, ipc, name.c_str(), event);
    // or if a name by
    //    fprintf(stdout, "%lld %lld %lld %lld %s\n", 
    //            mhz, duration, event, nameinsert, tempstring);
    //

    // Trace flag prints each incoming line and the resulting stack and span,
    // all on one line
    if (trace) {fprintf(stderr, "\n%s", buffer);}

    char name_buffer[256];
    // Pick off the event to see if it is a name definition line
    // (This could be done with less repeated effort)
    int64 temp_ts;
    uint64 temp_dur;
    int temp_eventnum = 0;
    int temp_arg = 0;
    char temp_name[64];
    sscanf(buffer, "%lld %llu %d %d %[ -~]", &temp_ts, &temp_dur, &temp_eventnum, &temp_arg, temp_name);
    if (IsNamedef(temp_eventnum)) {
//fprintf(stdout, "====%%%s\n", buffer);
      if (IsLockNameInt(temp_eventnum)) {		// Lock names
        locknames[temp_arg] = string(temp_name);
      } else if (IsKernelVerInt(temp_eventnum)) {
        kernel_version = string(temp_name);
        if (temp_ts == -1) {fprintf(stderr, "kernel_version = %s\n", temp_name);}
      } else if (IsModelNameInt(temp_eventnum)) {
        // If the model is Raspberry, set pstate_is_all_cpus
        if (strstr(temp_name, "Raspberry") != NULL) {
          is_rpi = true;
        }
        cpu_model_name = string(temp_name);
        if (temp_ts == -1) {fprintf(stderr, "cpu_model_name = %s\n", temp_name);}
      } else if (IsHostNameInt(temp_eventnum)) {
        host_name = string(temp_name);
        if (temp_ts == -1) {fprintf(stderr, "host_name = %s\n", temp_name);}
      ////} else if (IsUserExecNonidlenum(temp_arg)) {	// Just pick off PID names, accumulating if multiple ones
      } else if (IsPidNameInt(temp_eventnum)) {	// Just pick off PID names, accumulating if multiple ones
        RecordPidName(temp_ts, temp_arg, temp_name, cpustate);
        // Update any active stack if name just changed
        // Update any current span if name just changed
      } else if (IsMethodNameInt(temp_eventnum)) {
	// Step (0) of RPC-to-packet correlation
        int rpcid = temp_arg & 0xffff;
        methodnames[rpcid] = string(temp_name);
      } else if (IsQueueNameInt(temp_eventnum)) {
        queuenames[temp_arg] = string(temp_name);	// Queue number is a small integer
      }
      // Ignore the rest of the names -- already handled by rawtoevent and sort
      continue;
    } 

    // Read the full non-name event
    if (incoming_version < 2) {
      int n = sscanf(buffer, "%llu %llu %d %d %d %d %d %d %s",
                     &event.start_ts, &event.duration, &event.eventnum, &event.cpu, 
                     &event.pid, &event.rpcid, &event.arg, &event.retval, name_buffer);
      event.ipc = 0;
      if (n != 9) {continue;}
    } else {
      int n = sscanf(buffer, "%llu %llu %d %d %d %d %d %d %d %s",
                     &event.start_ts, &event.duration, &event.eventnum, &event.cpu, 
                     &event.pid, &event.rpcid, &event.arg, &event.retval,  
                     &event.ipc, name_buffer);
      if (n != 10) {continue;}
    }
    event.name = string(name_buffer);

    // Fix event.rpcid. rawtoevent does not carry them across context switches
    event.rpcid = cpustate[event.cpu].cpu_stack.rpcid;	// 2021.02.05

    // Fixup name of idle thread once and for all
    if (IsAnIdle(event)) {event.name = string(kIdleName);}

    // Input must be sorted by timestamp
    if (event.start_ts < prior_ts) {
      fprintf(stderr, "rawtoevent: Timestamp out of order at line[%d] %s\n", linenum, buffer);
      exit(0);
    }

if (verbose) {
fprintf(stdout, "\n%% [%d] %llu %llu %03x(%d)=%d %s ", 
        event.cpu, event.start_ts, event.duration, 
        event.eventnum, event.arg, event.retval, event.name.c_str());
DumpShort(stdout, &cpustate[event.cpu]);
}

    if ((lowest_ts == 0) && (0 < event.start_ts)) {
      lowest_ts = event.start_ts;
    }

    if (kMAX_CPUS <= event.cpu){
      fprintf(stderr, "FATAL: Too-big CPU number at line[%d] '%s'\n", linenum, buffer);
      exit(0);
    }

    // Keep track of largest CPU number seen
    if (max_cpu_seen < event.cpu) {
      max_cpu_seen = event.cpu;
    }

    // Fixup names
    FixNames(&event);

    // Fixup lock names
    if (IsALockOneSpan(event)) {
      char maybe_better_name[64];
      sprintf(maybe_better_name, "%s%s", 
              kSpecialName[event.eventnum & 0x001f], 
              locknames[event.arg].c_str());
      if (true || strlen(maybe_better_name) > strlen(name_buffer)) {
        // Do the replacement
//fprintf(stderr, "LOCK %d %s => %s\n", event.arg, name_buffer, maybe_better_name);
        event.name = string(maybe_better_name);
      }
    }

    // Fixup queue names, adding queue number if missing
    if (IsAnEnqueue(event) || IsADequeue(event)) {
      if (strchr(name_buffer, '(') == NULL) {
        char temp[64];
        sprintf(temp, "%s(%d)", name_buffer, event.arg);
        event.name = string(temp); 
      } 
    }

    // Collect RPC method names from RPC point events. TEMPORARY
    // DONE: capture proper RPC *method* names in the RPC library and pass them through
    // Just grab the first name, which should be on an rpc request event
/***
    if (IsAnRpc(event)) {
      // event.rpcid is always valid for RPC events
      if (rpcnames.find(event.rpcid) == rpcnames.end()) {
        if (false && event.name.find("rpc") == string::npos) {
          fprintf(stderr, "BAD rpcname %s\n", event.name.c_str());
          DumpEvent(stderr, "RPC", event);
        } else {
          rpcnames[event.rpcid] = event.name;
        }
      }
    }
***/

    prior_ts = event.start_ts;
    
    // Now do the real work
    PreProcessEvent(event, &cpustate[0], &perpidstate); 

    if (trace) {
      fprintf(stderr, "\t");
      CPUState* thiscpu = &cpustate[event.cpu];
      DumpStackShort(stderr, &thiscpu->cpu_stack);
    }   
  }
  //
  // End main loop
  //

  // Flush the last frequency spans here
  for (int i = 0; i <= max_cpu_seen; ++i) {
    if (cpustate[i].prior_pstate_ts != 0) {
      uint64 prior_ts = cpustate[i].prior_pstate_ts;
      uint64 prior_freq = cpustate[i].prior_pstate_freq;
      WriteFreqSpan(prior_ts, event.start_ts, i, prior_freq);
    }
  }
  
  // Keep any hardware description. Leading space is required.
  fprintf(stdout, " \"mbit_sec\" : %d,\n", mbit_sec);

  // Put out any multi-named PID row names
  for (IntName::const_iterator it = pidrownames.begin(); it != pidrownames.end(); ++it) {
    int pid = it->first;
    string rowname = it->second;
    double lowest_sec = lowest_ts / 100000000.0;
    if (rowname.find("+") != string::npos) {
      fprintf(stdout, "[%12.8f, %10.8f, %d, %d, %d, %d, %d, %d, %d, \"%s.%d\"],\n", 
          lowest_sec, 0.00000001, 0, pid, 0, KUTRACE_LEFTMARK, 0, 0, 0, rowname.c_str(), pid); 
    }
  }

  FinalJson(stdout);

  // Statistics for main timeline; no decorations, PCsamp, etc.
  double total_dur = total_usermode + total_idle + total_kernelmode;
  total_dur *= 0.01;	// To give percents
  fprintf(stderr, 
          "eventtospan3: %lld spans, %2.0f%% usr, %2.0f%% sys, %2.0f%% idle\n",
          span_count, 
          total_usermode / total_dur, total_kernelmode / total_dur, total_idle / total_dur);

  return 0;
}
