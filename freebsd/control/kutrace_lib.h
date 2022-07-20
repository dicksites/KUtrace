// kutrace_lib.h 
// Copyright 2022 Richard L. Sites
//
// This is a simple interface for user-mode code to control kernel/user tracing and 
// to add markers
//

#ifndef __KUTRACE_LIB_H__
#define __KUTRACE_LIB_H__

#include "basetypes.h"

typedef uint32 u32;
typedef uint64 u64;
typedef int64  s64;


typedef struct {
  int number;
  const char* name; 
} NumNamePair;


/* This is the definitive list of raw trace 12-bit event numbers */
// These user-mode declarations need to exactly match 
// source pool kutrace.h kernel-mode ones 

/* kutrace_control() commands */
#define KUTRACE_CMD_OFF 0
#define KUTRACE_CMD_ON 1
#define KUTRACE_CMD_FLUSH 2
#define KUTRACE_CMD_RESET 3
#define KUTRACE_CMD_STAT 4
#define KUTRACE_CMD_GETCOUNT 5
#define KUTRACE_CMD_GETWORD 6
#define KUTRACE_CMD_INSERT1 7
#define KUTRACE_CMD_INSERTN 8
#define KUTRACE_CMD_GETIPCWORD 9
#define KUTRACE_CMD_TEST 10
#define KUTRACE_CMD_VERSION 11



// All events are single uint64 entries unless otherwise specified
// +-------------------+-----------+---------------+-------+-------+
// | timestamp         | event     | delta | retval|      arg0     |
// +-------------------+-----------+---------------+-------+-------+
//          20              12         8       8           16 

// Add KUTRACE_ and uppercase
#define KUTRACE_NOP             0x000
#define KUTRACE_RDTSC           0x001	// unused
#define KUTRACE_GETTOD          0x002	// unused

#define KUTRACE_VARLENLO        0x010
#define KUTRACE_VARLENHI        0x1FF

// Variable-length starting numbers. Only events 010-1FF are variable length
// Middle hex digit of event number is 2..8, giving total length of entry including first uint64
// The arg is the lock# or PID# etc. that this name belongs to.
// +-------------------+-----------+-------------------------------+
// | timestamp         | event     |              arg              |
// +-------------------+-----------+-------------------------------+
// |  character name, 1-56 bytes, NUL padded                       |
// +- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -+
// ~                                                               ~
// +---------------------------------------------------------------+
//          20              12                    32 

// TimePair (DEFUNCT)
// +-------------------+-----------+-------------------------------+
// | timestamp         | event     |              arg              |
// +-------------------+-----------+-------------------------------+
// |   cycle counter value                                         |
// +---------------------------------------------------------------+
// |   matching gettimeofday value                                 |
// +---------------------------------------------------------------+
//          20              12                    32 


// Variable-length starting numbers. 
// Middle hex digit will become length in u64 words, 2..8
#define KUTRACE_FILENAME        0x001
#define KUTRACE_PIDNAME         0x002
#define KUTRACE_METHODNAME      0x003
#define KUTRACE_TRAPNAME        0x004
#define KUTRACE_INTERRUPTNAME   0x005
#define KUTRACE_TIMEPAIR        0x006	/* DEPRECATED */
#define KUTRACE_LOCKNAME        0x007	/* added 2019.10.25 */
#define KUTRACE_SYSCALL64NAME   0x008
#define KUTRACE_SYSCALL32NAME   0x00C
#define KUTRACE_ERRNONAME	0x00E
#define KUTRACE_PACKETNAME      0x100
#define KUTRACE_PC_TEMP         0x101	/* scaffolding 2020.01.29 now PC_U and PC_K */
#define KUTRACE_KERNEL_VER      0x102	/* Kernel version, uname -rv */
#define KUTRACE_MODEL_NAME      0x103	/* CPU model name, /proc/cpuinfo */
#define KUTRACE_HOST_NAME       0x104 	/* CPU host name */
#define KUTRACE_QUEUE_NAME      0x105 	/* Queue name */
#define KUTRACE_RES_NAME        0x106 	/* Arbitrary resource name */

// Specials are point events. Hex 200-220 currently. PC sample is outside this range
#define KUTRACE_USERPID         0x200	/* Context switch */
#define KUTRACE_RPCIDREQ        0x201	/* CPU is processing RPC# n request */
#define KUTRACE_RPCIDRESP       0x202	/* CPU is processing RPC# n response */
#define KUTRACE_RPCIDMID        0x203	/* CPU is processing RPC# n middle */
#define KUTRACE_RPCIDRXMSG      0x204	/* For display: RPC message received, approx packet time */
#define KUTRACE_RPCIDTXMSG      0x205	/* For display: RPC message sent, approx packet time */
#define KUTRACE_RUNNABLE        0x206	/* Make runnable */
#define KUTRACE_IPI             0x207	/* Send IPI */
#define KUTRACE_MWAIT           0x208	/* C-states: how deep to sleep */
#define KUTRACE_PSTATE          0x209	/* P-states: cpu freq sample in MHz increments */

// MARK_A,B,C arg is six base-40 chars NUL, A-Z, 0-9, . - /
// MARK_D     arg is unsigned int
// +-------------------+-----------+-------------------------------+
// | timestamp         | event     |              arg              |
// +-------------------+-----------+-------------------------------+
//          20              12                    32 

#define KUTRACE_MARKA           0x20A
#define KUTRACE_MARKB           0x20B
#define KUTRACE_MARKC           0x20C
#define KUTRACE_MARKD           0x20D
#define KUTRACE_LEFTMARK        0x20E	// Inserted by eventtospan
#define KUTRACE_RIGHTMARK       0x20F	// Inserted by eventtospan
#define KUTRACE_LOCKNOACQUIRE   0x210
#define KUTRACE_LOCKACQUIRE     0x211
#define KUTRACE_LOCKWAKEUP      0x212
        // unused               0x213
        
// Added 2020.10.29
#define KUTRACE_RX_PKT          0x214 	/* Raw packet received w/32-byte payload hash */ 
#define KUTRACE_TX_PKT          0x215 	/* Raw packet sent w/32-byte payload hash */

#define KUTRACE_RX_USER         0x216 	/* Request beginning at user code w/32-byte payload hash */ 
#define KUTRACE_TX_USER         0x217 	/* Response ending at user code w/32-byte payload hash */
  
#define KUTRACE_MBIT_SEC        0x218 	/* Network rate in Mb/s */

#define KUTRACE_RESOURCE	0x219  /* Arbitrary resource span; arg says which resource */
#define KUTRACE_ENQUEUE		0x21A  /* Put RPC on a work queue; arg says which queue */
#define KUTRACE_DEQUEUE		0x21B  /* Remove RPC from a queue; arg says which queue */
#define KUTRACE_PSTATE2         0x21C  /* P-states: cpu freq change, new in MHz increments */
// Added 2022.05.24
#define KUTRACE_TSDELTA         0x21D  /* Delta to advance timestamp */
#define KUTRACE_MONITORSTORE    0x21E  /* Store into a monitored location; does wakeup */
#define KUTRACE_MONITOREXIT     0x21F  /* Mwait exits due to store */

#define KUTRACE_MAX_SPECIAL     0x27F	// Last special, range 200..27F

// Extra events have duration, but are otherwise similar to specials
// PC sample. Not a special
#define KUTRACE_PC_U            0x280	/* added 2020.01.29 */
#define KUTRACE_PC_K            0x281	/* added 2020.02.01 */

// Lock held
#define KUTRACE_LOCK_HELD	0x282	/* Inserted by eventtospan 2020.09.27 */
#define KUTRACE_LOCK_TRY	0x283	/* Inserted by eventtospan 2020.09.27 */


/* Reasons for waiting, inserted only in postprocessing */
/* dsites 2019.10.25 */
#define KUTRACE_WAITA           0x300	/* a-z, through 0x0319 */
#define KUTRACE_WAITZ           0x319		

/* These are in blocks of 256 or 512 numbers */
#define KUTRACE_TRAP            0x400
#define KUTRACE_IRQ             0x500
#define KUTRACE_TRAPRET         0x600
#define KUTRACE_IRQRET          0x700
#define KUTRACE_SYSCALL64       0x800
#define KUTRACE_SYSRET64        0xA00
#define KUTRACE_SYSCALL32       0xC00
#define KUTRACE_SYSRET32        0xE00

/* Event numbers added in postprocessing or manually */
/*  -1 bracket, big } */
/*  -2 oval, fades out part of diagram */
/*  -3 arc, wakeup from one thread to another */
/*  -4 callout, bubble to label some event */
/*  -5 ... */

//
// These numbers must exactly match the numbers in kernel include file kutrace.h
//

/* Specific syscall numbers */
/* Take over last syscall32 number for tracing the scheduler call/return */
#define KUTRACE_SCHEDSYSCALL 1535	/* Top syscall32: 1023 + 512 */
	
/* Specific trap numbers */
#define KUTRACE_DNA		7	/* Device (8087) not available */
#define KUTRACE_PAGEFAULT	14

/* Specific IRQ numbers. Picked from arch/x86/include/asm/irq_vectors.h */
#define KUTRACE_LOCAL_TIMER_VECTOR	0xec

/* Reuse the spurious_apic vector to show bottom halves (AST) executing */
#define KUTRACE_BOTTOM_HALF	255
#define AST_SOFTIRQ		15

////#define RESCHEDULE_VECTOR      IPI_PREEMPT


// Names for events 000-00F could be added when one of these code points is
// actually used

// Names for the variable-length events 0y0-0yF and 1y0-1yF, where y is length in words 2..8
static const char* const kNameName[32] = {
  "-000-", "file", "pid", "rpc", 
  "trap", "irq", "trap", "irq",
  "syscall", "syscall", "syscall", "syscall",
  "syscall32", "syscall32", "errno", "syscall32",

  "packet", "pctmp", "kernv", "cpum",
  "host", "", "", "",
  "", "", "", "",
  "", "", "", "",
};

// Names for the special events 200-21F
static const char* const kSpecialName[32] = {
  "userpid", "rpcreq", "rpcresp", "rpcmid", 
  "rxmsg", "txmsg", "runnable", "sendipi",
  "mwait", "-freq-", "mark_a", "mark_b", 
  "mark_c", "mark_d", "-20e-", "-20f-", 
  "try_", "acq_", "rel_", "-213-",		// Locks
  "rx", "tx", "urx", "utx",
  "mbs", "res", "enq", "deq",
  "-21c-", "tsdelta", "mon_st", "mon_ex",
};

// Names for events 210-3FF could be added when one of these code points is
// actually used

// Names for events 400-FFF are always embedded in the trace

#if 1
// Common ERRNO names for FreeBSD
// If errno is in [-128..-1], subscript this by -errno - 1. 
// Error -1 EPERM thus maps to kErrnoName[0], not [1]
// Scraped from errno.h on 2022-07-03
// Plus $ cat tmp_errno.txt |sed 's/}.*$/, /' |sed 's/^[^,]*, //' >tmp_errno_sm.txt
// And some hand-edits for <CR>
static const char* const kErrnoName[128] = {
 "EPERM", "ENOENT", "ESRCH", "EINTR", "EIO", "ENXIO", "E2BIG", "ENOEXEC", 
 "EBADF", "ECHILD", "EDEADLK", "ENOMEM", "EACCES", "EFAULT", "ENOTBLK", "EBUSY", 
 "EEXIST", "EXDEV", "ENODEV", "ENOTDIR", "EISDIR", "EINVAL", "ENFILE", "EMFILE", 
 "ENOTTY", "ETXTBSY", "EFBIG", "ENOSPC", "ESPIPE", "EROFS", "EMLINK", "EPIPE", 

 "EDOM", "ERANGE", "EAGAIN", "EINPROGRESS", 
 "EALREADY", "ENOTSOCK", "EDESTADDRREQ", "EMSGSIZE", 
 "EPROTOTYPE", "ENOPROTOOPT", "EPROTONOSUPPORT", "ESOCKTNOSUPPORT", 
 "EOPNOTSUPP", "EPFNOSUPPORT", "EAFNOSUPPORT", "EADDRINUSE", 
 "EADDRNOTAVAIL", "ENETDOWN", "ENETUNREACH", "ENETRESET", 
 "ECONNABORTED", "ECONNRESET", "ENOBUFS", "EISCONN", 
 "ENOTCONN", "ESHUTDOWN", "ETOOMANYREFS", "ETIMEDOUT", 
 "ECONNREFUSED", "ELOOP", "ENAMETOOLONG", "EHOSTDOWN", 

 "EHOSTUNREACH", "ENOTEMPTY", "EPROCLIM", "EUSERS", 
 "EDQUOT", "ESTALE", "EREMOTE", "EBADRPC", 
 "ERPCMISMATCH", "EPROGUNAVAIL", "EPROGMISMATCH", "EPROCUNAVAIL", 
 "ENOLCK", "ENOSYS", "EFTYPE", "EAUTH", 
 "ENEEDAUTH", "EIDRM", "ENOMSG", "EOVERFLOW", 
 "ECANCELED", "EILSEQ", "ENOATTR", "EDOOFUS", 
 "EBADMSG", "EMULTIHOP", "ENOLINK", "EPROTO", 
 "ENOTCAPABLE", "ECAPMODE", "ENOTRECOVERABLE", "EOWNERDEAD", 

 "EINTEGRITY", "", "", "", "", "", "", "", 
  "", "", "", "", "", "", "", "", 
  "", "", "", "", "", "", "", "", 
  "", "", "", "", "", "", "", "", 
};

#else

// Common ERRNO names for Linux
// x86- and ARM-specific Names for return codes -128 to -1
// If errno is in [-128..-1], subscript this by -errno - 1. 
// Error -1 EPERM thus maps to kErrnoName[0], not [1]
// See include/uapi/asm-generic/errno-base.h
// See include/uapi/asm-generic/errno.h
// ...more could be added
static const char* const kErrnoName[128] = {
  "EPERM", "ENOENT", "ESRCH", "EINTR", "EIO", "ENXIO", "E2BIG", "ENOEXEC",
  "EBADF", "ECHILD", "EAGAIN", "ENOMEM", "EACCES", "EFAULT", "ENOTBLK", "EBUSY",
  "EEXIST", "EXDEV", "ENODEV", "ENOTDIR", "EISDIR", "EINVAL", "ENFILE", "EMFILE",
  "ENOTTY", "ETXTBSY", "EFBIG", "ENOSPC", "ESPIPE", "EROFS", "EMLINK", "EPIPE",

  "EDOM", "ERANGE", "EDEADLK", "ENAMETOOLONG", "ENOLCK", "ENOSYS", "ENOTEMPTY", "ELOOP", 
  "", "ENOMSG", "EIDRM", "ECHRNG", "EL2NSYNC", "EL3HLT", "EL3RST", "ELNRNG", 
  "EUNATCH", "ENOCSI", "EL2HLT", "EBADE", "EBADR", "EXFULL", "ENOANO", "EBADRQC", 
  "EBADSLT", "", "EBFONT", "ENOSTR", "ENODATA", "ETIME", "ENOSR", "ENONET", 

  "", "", "", "", "", "", "", "", 
  "", "", "", "", "", "", "", "", 
  "", "", "", "", "", "", "", "", 
  "", "", "", "", "", "", "", "", 

  "", "", "", "", "", "", "", "", 
  "", "", "", "", "", "", "", "", 
  "", "", "", "", "", "", "", "", 
  "", "", "", "", "", "", "", "", 
};

#endif

namespace kutrace {
  bool test();
  void go(const char* process_name);
  void goipc(const char* process_name);
  void stop(const char* fname);
  void mark_a(const char* label);
  void mark_b(const char* label);
  void mark_c(const char* label);
  void mark_d(u64 n);

  // Returns number of words inserted 1..8, or
  //   0 if tracing is off, negative if module is not not loaded 
  u64 addevent(u64 eventnum, u64 arg);
  void addname(u64 eventnum, u64 number, const char* name);

  void msleep(int msec);
  int64 readtime();

  const char* Base40ToChar(u64 base40, char* str);
  u64 CharToBase40(const char* str);

  u64 DoControl(u64 command, u64 arg);
  void DoDump(const char* fname);
  u64 DoEvent(u64 eventnum, u64 arg);
  void DoFlush();
  void DoInit(const char* process_name);
  void DoMark(u64 n, u64 arg);
  bool DoTest();
  bool DoOff();
  bool DoOn();
  void DoQuit();
  void DoReset(u64 doing_ipc);
  void DoStat(u64 control_flags);
  void EmitNames(const NumNamePair* ipair, u64 n);
  u64 GetUsec();
  const char* MakeTraceFileName(const char* name, char* str);
  bool TestModule();
}

#endif	// __KUTRACE_LIB_H__


