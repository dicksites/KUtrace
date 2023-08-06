// Names for syscall, etc. in KUtrace 
// dick sites 2022.07.03 FreeBSD 14.0 version
//

/*
 * Copyright (C) 2022 Richard L. Sites
 *
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 */

#ifndef __KUTRACE_CONTROL_NAMES_H__
#define __KUTRACE_CONTROL_NAMES_H__

#include "kutrace_lib.h"

static const NumNamePair PidNames[] = {
  {0, "-idle-"},
  {-1, NULL},		// Must be last
};

/* dsites 2022.06.07 switch to no longer use kutrace_syscall_map table */
/*   (syscalls are mapped into two discontiguous ranges, 0-511 and 1024-1535) */
/* See scrape_syscall.sh */
static const NumNamePair Syscall64Names[] = {
  // Scraped from /usr/src/sys/sys/syscall.h on 2022-07-03
  {0, "syscall"},
  {1, "exit"},
  {2, "fork"},
  {3, "read"},
  {4, "write"},
  {5, "open"},
  {6, "close"},
  {7, "wait4"},
  {9, "link"},
  {10, "unlink"},
  {12, "chdir"},
  {13, "fchdir"},
  {14, "freebsd11_mknod"},
  {15, "chmod"},
  {16, "chown"},
  {17, "break"},
  {20, "getpid"},
  {21, "mount"},
  {22, "unmount"},
  {23, "setuid"},
  {24, "getuid"},
  {25, "geteuid"},
  {26, "ptrace"},
  {27, "recvmsg"},
  {28, "sendmsg"},
  {29, "recvfrom"},
  {30, "accept"},
  {31, "getpeername"},
  {32, "getsockname"},
  {33, "access"},
  {34, "chflags"},
  {35, "fchflags"},
  {36, "sync"},
  {37, "kill"},
  {39, "getppid"},
  {41, "dup"},
  {42, "freebsd10_pipe"},
  {43, "getegid"},
  {44, "profil"},
  {45, "ktrace"},
  {47, "getgid"},
  {49, "getlogin"},
  {50, "setlogin"},
  {51, "acct"},
  {53, "sigaltstack"},
  {54, "ioctl"},
  {55, "reboot"},
  {56, "revoke"},
  {57, "symlink"},
  {58, "readlink"},
  {59, "execve"},
  {60, "umask"},
  {61, "chroot"},
  {65, "msync"},
  {66, "vfork"},
  {69, "sbrk"},
  {70, "sstk"},
  {72, "freebsd11_vadvise"},
  {73, "munmap"},
  {74, "mprotect"},
  {75, "madvise"},
  {78, "mincore"},
  {79, "getgroups"},
  {80, "setgroups"},
  {81, "getpgrp"},
  {82, "setpgid"},
  {83, "setitimer"},
  {85, "swapon"},
  {86, "getitimer"},
  {89, "getdtablesize"},
  {90, "dup2"},
  {92, "fcntl"},
  {93, "select"},
  {95, "fsync"},
  {96, "setpriority"},
  {97, "socket"},
  {98, "connect"},
  {100, "getpriority"},
  {104, "bind"},
  {105, "setsockopt"},
  {106, "listen"},
  {116, "gettimeofday"},
  {117, "getrusage"},
  {118, "getsockopt"},
  {120, "readv"},
  {121, "writev"},
  {122, "settimeofday"},
  {123, "fchown"},
  {124, "fchmod"},
  {126, "setreuid"},
  {127, "setregid"},
  {128, "rename"},
  {131, "flock"},
  {132, "mkfifo"},
  {133, "sendto"},
  {134, "shutdown"},
  {135, "socketpair"},
  {136, "mkdir"},
  {137, "rmdir"},
  {138, "utimes"},
  {140, "adjtime"},
  {147, "setsid"},
  {148, "quotactl"},
  {154, "nlm_syscall"},
  {155, "nfssvc"},
  {160, "lgetfh"},
  {161, "getfh"},
  {165, "sysarch"},
  {166, "rtprio"},
  {169, "semsys"},
  {170, "msgsys"},
  {171, "shmsys"},
  {175, "setfib"},
  {176, "ntp_adjtime"},
  {181, "setgid"},
  {182, "setegid"},
  {183, "seteuid"},
  {188, "freebsd11_stat"},
  {189, "freebsd11_fstat"},
  {190, "freebsd11_lstat"},
  {191, "pathconf"},
  {192, "fpathconf"},
  {194, "getrlimit"},
  {195, "setrlimit"},
  {196, "freebsd11_getdirentries"},
  {198, "__syscall"},
  {202, "__sysctl"},
  {203, "mlock"},
  {204, "munlock"},
  {205, "undelete"},
  {206, "futimes"},
  {207, "getpgid"},
  {209, "poll"},
  {220, "freebsd7___semctl"},
  {221, "semget"},
  {222, "semop"},
  {224, "freebsd7_msgctl"},
  {225, "msgget"},
  {226, "msgsnd"},
  {227, "msgrcv"},
  {228, "shmat"},
  {229, "freebsd7_shmctl"},
  {230, "shmdt"},
  {231, "shmget"},
  {232, "clock_gettime"},
  {233, "clock_settime"},
  {234, "clock_getres"},
  {235, "ktimer_create"},
  {236, "ktimer_delete"},
  {237, "ktimer_settime"},
  {238, "ktimer_gettime"},
  {239, "ktimer_getoverrun"},
  {240, "nanosleep"},
  {241, "ffclock_getcounter"},
  {242, "ffclock_setestimate"},
  {243, "ffclock_getestimate"},
  {244, "clock_nanosleep"},
  {247, "clock_getcpuclockid2"},
  {248, "ntp_gettime"},
  {250, "minherit"},
  {251, "rfork"},
  {253, "issetugid"},
  {254, "lchown"},
  {255, "aio_read"},
  {256, "aio_write"},
  {257, "lio_listio"},
  {272, "freebsd11_getdents"},
  {274, "lchmod"},
  {276, "lutimes"},
  {278, "freebsd11_nstat"},
  {279, "freebsd11_nfstat"},
  {280, "freebsd11_nlstat"},
  {289, "preadv"},
  {290, "pwritev"},
  {298, "fhopen"},
  {299, "freebsd11_fhstat"},
  {300, "modnext"},
  {301, "modstat"},
  {302, "modfnext"},
  {303, "modfind"},
  {304, "kldload"},
  {305, "kldunload"},
  {306, "kldfind"},
  {307, "kldnext"},
  {308, "kldstat"},
  {309, "kldfirstmod"},
  {310, "getsid"},
  {311, "setresuid"},
  {312, "setresgid"},
  {314, "aio_return"},
  {315, "aio_suspend"},
  {316, "aio_cancel"},
  {317, "aio_error"},
  {321, "yield"},
  {324, "mlockall"},
  {325, "munlockall"},
  {326, "__getcwd"},
  {327, "sched_setparam"},
  {328, "sched_getparam"},
  {329, "sched_setscheduler"},
  {330, "sched_getscheduler"},
  {331, "sched_yield"},
  {332, "sched_get_priority_max"},
  {333, "sched_get_priority_min"},
  {334, "sched_rr_get_interval"},
  {335, "utrace"},
  {337, "kldsym"},
  {338, "jail"},
  {339, "nnpfs_syscall"},
  {340, "sigprocmask"},
  {341, "sigsuspend"},
  {343, "sigpending"},
  {345, "sigtimedwait"},
  {346, "sigwaitinfo"},
  {347, "__acl_get_file"},
  {348, "__acl_set_file"},
  {349, "__acl_get_fd"},
  {350, "__acl_set_fd"},
  {351, "__acl_delete_file"},
  {352, "__acl_delete_fd"},
  {353, "__acl_aclcheck_file"},
  {354, "__acl_aclcheck_fd"},
  {355, "extattrctl"},
  {356, "extattr_set_file"},
  {357, "extattr_get_file"},
  {358, "extattr_delete_file"},
  {359, "aio_waitcomplete"},
  {360, "getresuid"},
  {361, "getresgid"},
  {362, "kqueue"},
  {363, "freebsd11_kevent"},
  {371, "extattr_set_fd"},
  {372, "extattr_get_fd"},
  {373, "extattr_delete_fd"},
  {374, "__setugid"},
  {376, "eaccess"},
  {377, "afs3_syscall"},
  {378, "nmount"},
  {384, "__mac_get_proc"},
  {385, "__mac_set_proc"},
  {386, "__mac_get_fd"},
  {387, "__mac_get_file"},
  {388, "__mac_set_fd"},
  {389, "__mac_set_file"},
  {390, "kenv"},
  {391, "lchflags"},
  {392, "uuidgen"},
  {393, "sendfile"},
  {394, "mac_syscall"},
  {395, "freebsd11_getfsstat"},
  {396, "freebsd11_statfs"},
  {397, "freebsd11_fstatfs"},
  {398, "freebsd11_fhstatfs"},
  {400, "ksem_close"},
  {401, "ksem_post"},
  {402, "ksem_wait"},
  {403, "ksem_trywait"},
  {404, "ksem_init"},
  {405, "ksem_open"},
  {406, "ksem_unlink"},
  {407, "ksem_getvalue"},
  {408, "ksem_destroy"},
  {409, "__mac_get_pid"},
  {410, "__mac_get_link"},
  {411, "__mac_set_link"},
  {412, "extattr_set_link"},
  {413, "extattr_get_link"},
  {414, "extattr_delete_link"},
  {415, "__mac_execve"},
  {416, "sigaction"},
  {417, "sigreturn"},
  {421, "getcontext"},
  {422, "setcontext"},
  {423, "swapcontext"},
  {424, "freebsd13_swapoff"},
  {425, "__acl_get_link"},
  {426, "__acl_set_link"},
  {427, "__acl_delete_link"},
  {428, "__acl_aclcheck_link"},
  {429, "sigwait"},
  {430, "thr_create"},
  {431, "thr_exit"},
  {432, "thr_self"},
  {433, "thr_kill"},
  {434, "freebsd10__umtx_lock"},
  {435, "freebsd10__umtx_unlock"},
  {436, "jail_attach"},
  {437, "extattr_list_fd"},
  {438, "extattr_list_file"},
  {439, "extattr_list_link"},
  {441, "ksem_timedwait"},
  {442, "thr_suspend"},
  {443, "thr_wake"},
  {444, "kldunloadf"},
  {445, "audit"},
  {446, "auditon"},
  {447, "getauid"},
  {448, "setauid"},
  {449, "getaudit"},
  {450, "setaudit"},
  {451, "getaudit_addr"},
  {452, "setaudit_addr"},
  {453, "auditctl"},
  {454, "_umtx_op"},
  {455, "thr_new"},
  {456, "sigqueue"},
  {457, "kmq_open"},
  {458, "kmq_setattr"},
  {459, "kmq_timedreceive"},
  {460, "kmq_timedsend"},
  {461, "kmq_notify"},
  {462, "kmq_unlink"},
  {463, "abort2"},
  {464, "thr_set_name"},
  {465, "aio_fsync"},
  {466, "rtprio_thread"},
  {471, "sctp_peeloff"},
  {472, "sctp_generic_sendmsg"},
  {473, "sctp_generic_sendmsg_iov"},
  {474, "sctp_generic_recvmsg"},
  {475, "pread"},
  {476, "pwrite"},
  {477, "mmap"},
  {478, "lseek"},
  {479, "truncate"},
  {480, "ftruncate"},
  {481, "thr_kill2"},
  {482, "freebsd12_shm_open"},
  {483, "shm_unlink"},
  {484, "cpuset"},
  {485, "cpuset_setid"},
  {486, "cpuset_getid"},
  {487, "cpuset_getaffinity"},
  {488, "cpuset_setaffinity"},
  {489, "faccessat"},
  {490, "fchmodat"},
  {491, "fchownat"},
  {492, "fexecve"},
  {493, "freebsd11_fstatat"},
  {494, "futimesat"},
  {495, "linkat"},
  {496, "mkdirat"},
  {497, "mkfifoat"},
  {498, "freebsd11_mknodat"},
  {499, "openat"},
  {500, "readlinkat"},
  {501, "renameat"},
  {502, "symlinkat"},
  {503, "unlinkat"},
  {504, "posix_openpt"},
  {505, "gssd_syscall"},
  {506, "jail_get"},
  {507, "jail_set"},
  {508, "jail_remove"},
  {509, "freebsd12_closefrom"},
  {510, "__semctl"},
  {511, "msgctl"},
  {1024, "shmctl"},
  {1025, "lpathconf"},
  {1027, "__cap_rights_get"},
  {1028, "cap_enter"},
  {1029, "cap_getmode"},
  {1030, "pdfork"},
  {1031, "pdkill"},
  {1032, "pdgetpid"},
  {1034, "pselect"},
  {1035, "getloginclass"},
  {1036, "setloginclass"},
  {1037, "rctl_get_racct"},
  {1038, "rctl_get_rules"},
  {1039, "rctl_get_limits"},
  {1040, "rctl_add_rule"},
  {1041, "rctl_remove_rule"},
  {1042, "posix_fallocate"},
  {1043, "posix_fadvise"},
  {1044, "wait6"},
  {1045, "cap_rights_limit"},
  {1046, "cap_ioctls_limit"},
  {1047, "cap_ioctls_get"},
  {1048, "cap_fcntls_limit"},
  {1049, "cap_fcntls_get"},
  {1050, "bindat"},
  {1051, "connectat"},
  {1052, "chflagsat"},
  {1053, "accept4"},
  {1054, "pipe2"},
  {1055, "aio_mlock"},
  {1056, "procctl"},
  {1057, "ppoll"},
  {1058, "futimens"},
  {1059, "utimensat"},
  {1062, "fdatasync"},
  {1063, "fstat"},
  {1064, "fstatat"},
  {1065, "fhstat"},
  {1066, "getdirentries"},
  {1067, "statfs"},
  {1068, "fstatfs"},
  {1069, "getfsstat"},
  {1070, "fhstatfs"},
  {1071, "mknodat"},
  {1072, "kevent"},
  {1073, "cpuset_getdomain"},
  {1074, "cpuset_setdomain"},
  {1075, "getrandom"},
  {1076, "getfhat"},
  {1077, "fhlink"},
  {1078, "fhlinkat"},
  {1079, "fhreadlink"},
  {1080, "funlinkat"},
  {1081, "copy_file_range"},
  {1082, "__sysctlbyname"},
  {1083, "shm_open2"},
  {1084, "shm_rename"},
  {1085, "sigfastblock"},
  {1086, "__realpathat"},
  {1087, "close_range"},
  {1088, "rpctls_syscall"},
  {1089, "__specialfd"},
  {1090, "aio_writev"},
  {1091, "aio_readv"},
  {1092, "fspacectl"},
  {1093, "sched_getcpu"},
  {1094, "swapoff"},
  {1095, "MAXSYSCALL"},
  
  {210, "lkmnosys"},    // Hand inserted. Loadable module (e.g. KUtrace control syscall)
  {211, "lkmnosys"},
  {212, "lkmnosys"},
  {213, "lkmnosys"},
  {214, "lkmnosys"},
  {215, "lkmnosys"},
  {216, "lkmnosys"},
  {217, "lkmnosys"},
  {218, "lkmnosys"},
  {219, "lkmnosys"},
  {1535, "-sched-"},	// Fake last syscall. Indicates where __schedule runs
  {-1, NULL},		// Must be last
};

// VARIES by machine, general x86 layout here
/*
 *  Vectors   0-31  : system traps and exceptions - hardcoded events
 *  Vector    0     :  legacy ISA timer
 *            1     :  legacy keyboard
 *            2     :  legacy ??
 *            3     :  legacy serial port #1
 *            4     :  legacy serial port #2
 *            5     :  legacy ??
 *            6     :  legacy ??
 *            7     :  legacy printer port
 *            8     :  legacy real-time clock or floppy disk 
 *            9-11  :  legacy ?? 
 *           12     :  legacy PS/2 mouse
 *           13     :  legacy FP coprocessor
 *           14     :  legacy IDE controller #1
 *           15     :  legacy IDE controller #2
 *  Vectors  32-127 : device interrupts
 *           32-39  :  legacy 8259A #1
 *           40-47  :  legacy 8259A #2
 *  Vector  128     :  legacy int80 syscall interface
 *  Vectors 129-238 : device interrupts
 *  Vectors 239     : local timer interrupt 
 *  Vectors 240-255 : IPIs in FreeBSD
 */

/* See scrape_irq_freebsd.sh */
static const NumNamePair IrqNames[] = {
  // Scraped from vmstat -ia on 2022-07-03
  {1, "atkbd0"},
  {0, "attimer0"},
  {4, "uart0"},
  {8, "atrtc0"},
  {9, "acpi0"},
  {16, "ichsmb0"},
  {120, "hpet0:t0"},
  {121, "hpet0:t1"},
  {122, "hpet0:t2"},
  {123, "hpet0:t3"},
  {124, "hpet0:t4"},
  {125, "hpet0:t5"},
  {126, "hpet0:t6"},
  {127, "hpet0:t7"},
  {128, "xhci0"},
  {129, "ahci0"},
  {130, "nvme0:admin"},
  {131, "nvme0:io0"},
  {132, "nvme0:io1"},
  {133, "re0"},
  {134, "hdac0"},

  // Inserted by hand for FreeBSD
  {236, "local_timer_vector"},		// dsites 2022.06.01
  {248, "resched_ipi"},			// dsites 2022.06.01
  {-1, NULL},		// Must be last
};

// Bottom half BH vectors, from include/linux/interrupt.h
// UNUSED in FreeBSD
static const NumNamePair SoftIrqNames[] = {
  {0, "HI_SOFTIRQ"},
  {1, "TIMER_SOFTIRQ"},
  {2, "NET_TX_SOFTIRQ"},
  {3, "NET_RX_SOFTIRQ"},
  {4, "BLOCK_SOFTIRQ"},
  {5, "IRQ_POLL_SOFTIRQ"},
  {6, "TASKLET_SOFTIRQ"},
  {7, "SCHED_SOFTIRQ"},
  {8, "HRTIMER_SOFTIRQ"},
  {9, "RCU_SOFTIRQ"},
  {15,"AST_SOFTIRQ"},
  {-1, NULL},		// Must be last
};

// Only dev_not_avail and page_fault are used in KUtrace
static const NumNamePair TrapNames[] = {
  {0, "Divide-by-zero"},
  {1, "Debug"},
  {2, "Non-maskable_Interrupt"},
  {3, "Breakpoint"},
  {4, "Overflow"},
  {5, "Bound_Range_Exceeded"},
  {6, "Invalid_Opcode"},
  {7, "dev_not_avail"},			// Used
  {8, "Double_Fault"},
  {9, "Coprocessor_Segment_Overrun"},
  {10, "Invalid_TSS"},
  {11, "Segment_Not_Present"},
  {12, "Stack_Segment_Fault"},
  {13, "General_Protection_Fault"},
  {14, "page_fault"},			// Used
  {15, "Spurious_Interrupt"},
  {16, "x87_Floating-Point_Exception"},
  {17, "Alignment_Check"},
  {18, "Machine_Check"},
  {19, "SIMD_Floating-Point_Exception"},
  {32, "IRET_Exception"},
  {-1, NULL},		// Must be last
};

/* See scrape_errno_freebsd.sh */
static const NumNamePair ErrnoNames[] = {
  // Scraped from /usr/src/sys/sys/errno.h on 2022-07-03
  {1, "EPERM"},  /* Operation not permitted */
  {2, "ENOENT"},  /* No such file or directory */
  {3, "ESRCH"},  /* No such process */
  {4, "EINTR"},  /* Interrupted system call */
  {5, "EIO"},  /* Input/output error */
  {6, "ENXIO"},  /* Device not configured */
  {7, "E2BIG"},  /* Argument list too long */
  {8, "ENOEXEC"},  /* Exec format error */
  {9, "EBADF"},  /* Bad file descriptor */
  {10, "ECHILD"},  /* No child processes */
  {11, "EDEADLK"},  /* Resource deadlock avoided */
  {12, "ENOMEM"},  /* Cannot allocate memory */
  {13, "EACCES"},  /* Permission denied */
  {14, "EFAULT"},  /* Bad address */
  {15, "ENOTBLK"},  /* Block device required */
  {16, "EBUSY"},  /* Device busy */
  {17, "EEXIST"},  /* File exists */
  {18, "EXDEV"},  /* Cross-device link */
  {19, "ENODEV"},  /* Operation not supported by device */
  {20, "ENOTDIR"},  /* Not a directory */
  {21, "EISDIR"},  /* Is a directory */
  {22, "EINVAL"},  /* Invalid argument */
  {23, "ENFILE"},  /* Too many open files in system */
  {24, "EMFILE"},  /* Too many open files */
  {25, "ENOTTY"},  /* Inappropriate ioctl for device */
  {26, "ETXTBSY"},  /* Text file busy */
  {27, "EFBIG"},  /* File too large */
  {28, "ENOSPC"},  /* No space left on device */
  {29, "ESPIPE"},  /* Illegal seek */
  {30, "EROFS"},  /* Read-only filesystem */
  {31, "EMLINK"},  /* Too many links */
  {32, "EPIPE"},  /* Broken pipe */
  {33, "EDOM"},  /* Numerical argument out of domain */
  {34, "ERANGE"},  /* Result too large */
  {35, "EAGAIN"},  /* Resource temporarily unavailable */
  {36, "EINPROGRESS"},  /* Operation now in progress */
  {37, "EALREADY"},  /* Operation already in progress */
  {38, "ENOTSOCK"},  /* Socket operation on non-socket */
  {39, "EDESTADDRREQ"},  /* Destination address required */
  {40, "EMSGSIZE"},  /* Message too long */
  {41, "EPROTOTYPE"},  /* Protocol wrong type for socket */
  {42, "ENOPROTOOPT"},  /* Protocol not available */
  {43, "EPROTONOSUPPORT"},  /* Protocol not supported */
  {44, "ESOCKTNOSUPPORT"},  /* Socket type not supported */
  {45, "EOPNOTSUPP"},  /* Operation not supported */
  {46, "EPFNOSUPPORT"},  /* Protocol family not supported */
  {47, "EAFNOSUPPORT"},  /* Address family not supported by protocol family */
  {48, "EADDRINUSE"},  /* Address already in use */
  {49, "EADDRNOTAVAIL"},  /* Can't assign requested address */
  {50, "ENETDOWN"},  /* Network is down */
  {51, "ENETUNREACH"},  /* Network is unreachable */
  {52, "ENETRESET"},  /* Network dropped connection on reset */
  {53, "ECONNABORTED"},  /* Software caused connection abort */
  {54, "ECONNRESET"},  /* Connection reset by peer */
  {55, "ENOBUFS"},  /* No buffer space available */
  {56, "EISCONN"},  /* Socket is already connected */
  {57, "ENOTCONN"},  /* Socket is not connected */
  {58, "ESHUTDOWN"},  /* Can't send after socket shutdown */
  {59, "ETOOMANYREFS"},  /* Too many references: can't splice */
  {60, "ETIMEDOUT"},  /* Operation timed out */
  {61, "ECONNREFUSED"},  /* Connection refused */
  {62, "ELOOP"},  /* Too many levels of symbolic links */
  {63, "ENAMETOOLONG"},  /* File name too long */
  {64, "EHOSTDOWN"},  /* Host is down */
  {65, "EHOSTUNREACH"},  /* No route to host */
  {66, "ENOTEMPTY"},  /* Directory not empty */
  {67, "EPROCLIM"},  /* Too many processes */
  {68, "EUSERS"},  /* Too many users */
  {69, "EDQUOT"},  /* Disc quota exceeded */
  {70, "ESTALE"},  /* Stale NFS file handle */
  {71, "EREMOTE"},  /* Too many levels of remote in path */
  {72, "EBADRPC"},  /* RPC struct is bad */
  {73, "ERPCMISMATCH"},  /* RPC version wrong */
  {74, "EPROGUNAVAIL"},  /* RPC prog. not avail */
  {75, "EPROGMISMATCH"},  /* Program version wrong */
  {76, "EPROCUNAVAIL"},  /* Bad procedure for program */
  {77, "ENOLCK"},  /* No locks available */
  {78, "ENOSYS"},  /* Function not implemented */
  {79, "EFTYPE"},  /* Inappropriate file type or format */
  {80, "EAUTH"},  /* Authentication error */
  {81, "ENEEDAUTH"},  /* Need authenticator */
  {82, "EIDRM"},  /* Identifier removed */
  {83, "ENOMSG"},  /* No message of desired type */
  {84, "EOVERFLOW"},  /* Value too large to be stored in data type */
  {85, "ECANCELED"},  /* Operation canceled */
  {86, "EILSEQ"},  /* Illegal byte sequence */
  {87, "ENOATTR"},  /* Attribute not found */
  {88, "EDOOFUS"},  /* Programming error */
  {89, "EBADMSG"},  /* Bad message */
  {90, "EMULTIHOP"},  /* Multihop attempted */
  {91, "ENOLINK"},  /* Link has been severed */
  {92, "EPROTO"},  /* Protocol error */
  {93, "ENOTCAPABLE"},  /* Capabilities insufficient */
  {94, "ECAPMODE"},  /* Not permitted in capability mode */
  {95, "ENOTRECOVERABLE"},  /* State not recoverable */
  {96, "EOWNERDEAD"},  /* Previous owner died */
  {97, "EINTEGRITY"},  /* Integrity check failed */
  {97, "ELAST"},  /* Must be equal largest errno */
  {-1, NULL},		// Must be last
};
#endif	// __KUTRACE_CONTROL_NAMES_H__


