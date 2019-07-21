// kutrace_lib.h 
//
// This is a simple interface for user-mode code to control kernel/user tracing and add markers
//

/*
 * Copyright (C) 2019 Richard L. Sites
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef __KUTRACE_LIB_H__
#define __KUTRACE_LIB_H__

#include "basetypes.h"

typedef long unsigned int u64;

typedef struct {
  int number;
  const char* name; 
} NumNamePair;


/* This is the definitive list of raw trace 12-bit event numbers */
// These user-mode declarations need to exactly match include/linux/kutrace.h kernel-mode ones 

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
#define KUTRACE_NOP           0x000
#define KUTRACE_RDTSC         0x001	// unused
#define KUTRACE_GETTOD        0x002	// unused

#define KUTRACE_VARLENLO      0x010
#define KUTRACE_VARLENHI      0x1FF

// Variable-length starting numbers. Only events 010-1FF are variable length
// Middle hex digit of event number is 2..8, giving total length of entry including first uint64
// The arg is the lock# or PID# etc. that this name belongs to.
// +-------------------+-----------+-------------------------------+
// | timestamp         | event     |              arg              |
// +-------------------+-----------+-------------------------------+
// |  character name, 1-56 bytes, NUL padded                       |
// +---------------------------------------------------------------+
// ~                                                               ~
// +---------------------------------------------------------------+
//          20              12                    32 

// TimePair
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
#define KUTRACE_FILENAME      0x001
#define KUTRACE_PIDNAME       0x002
#define KUTRACE_METHODNAME    0x003
#define KUTRACE_TRAPNAME      0x004
#define KUTRACE_INTERRUPTNAME 0x005
#define KUTRACE_TIMEPAIR      0x006
// 0x007 available
#define KUTRACE_SYSCALL64NAME 0x008
#define KUTRACE_SYSCALL32NAME 0x00C
#define KUTRACE_PACKETNAME    0x100

// Specials are point events
#define KUTRACE_USERPID       0x200
#define KUTRACE_RPCIDREQ      0x201
#define KUTRACE_RPCIDRESP     0x202
#define KUTRACE_RPCIDMID      0x203
#define KUTRACE_RPCIDRXPKT    0x204
#define KUTRACE_RPCIDTXPKT    0x205
#define KUTRACE_RUNNABLE      0x206
#define KUTRACE_IPI           0x207
#define KUTRACE_MWAIT         0x208	/* C-states */
#define KUTRACE_PSTATE        0x209	/* P-states */


// MARK_A,B,C arg is six base-40 chars NUL, A-Z, 0-9, . - /
// MARK_D     arg is unsigned int
// +-------------------+-----------+-------------------------------+
// | timestamp         | event     |              arg              |
// +-------------------+-----------+-------------------------------+
//          20              12                    32 

#define KUTRACE_MARKA            0x20A
#define KUTRACE_MARKB            0x20B
#define KUTRACE_MARKC            0x20C
#define KUTRACE_MARKD            0x20D
  // available           0x20E
  // available           0x20F
#define KUTRACE_LOCKNOACQUIRE    0x210
#define KUTRACE_LOCKACQUIRE      0x211
#define KUTRACE_LOCKWAKEUP       0x212

/* These are in blocks of 256 or 512 numbers */
#define KUTRACE_TRAP      0x0400
#define KUTRACE_IRQ       0x0500
#define KUTRACE_TRAPRET   0x0600
#define KUTRACE_IRQRET    0x0700
#define KUTRACE_SYSCALL64 0x0800
#define KUTRACE_SYSRET64  0x0A00
#define KUTRACE_SYSCALL32 0x0C00
#define KUTRACE_SYSRET32  0x0E00


// Names for events 000-00F could be added when one of these code points is
// actually used

// Names for the variable-length events 0y0-0yF and 1y0-1yF, where y is length in words 2..8
static const char* const kNameName[32] = {
  "-000-", "file", "pid", "rpc", 
  "trap", "irq", "trap", "irq",
  "syscall", "syscall", "syscall", "syscall",
  "syscall32", "syscall32", "syscall32", "syscall32",

  "packet", "", "", "",
  "", "", "", "",
  "", "", "", "",
  "", "", "", "",
};

// Names for the special events 200-20F
static const char* const kSpecialName[16] = {
  "userpid", "rpcreq", "rpcresp", "rpcmid", 
  "rpcrxpkt", "rpxtxpkt", "runnable", "sendipi",
  "mwait", "lockwait", "mark_a", "mark_b", 
  "mark_c", "mark_d", "-20e-", "-20f-", 
};

// Names for events 210-3FF could be added when one of these code points is
// actually used

// Names for events 400-FFF are always embedded in the trace

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


namespace kutrace {
  bool test();
  void go(const char* process_name);
  void goipc(const char* process_name);
  void stop(const char* fname);
  void mark_a(const char* label);
  void mark_b(const char* label);
  void mark_c(const char* label);
  void mark_d(unsigned long int n);

  void addevent(unsigned long int eventnum, unsigned long int arg);
  void msleep(int msec);
  int64 readtime();

  const char* Base40ToChar(u64 base40, char* str);
  u64 CharToBase40(const char* str);

  void DoControl(u64 command, u64 arg);
  void DoDump(const char* fname);
  void DoEvent(u64 eventnum, u64 arg);
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
  void GetUsec();
  const char* MakeTraceFileName(const char* name, char* str);
  bool TestModule();
}


#endif	// __KUTRACE_LIB_H__


