// Names for syscall, etc. in dclab_tracing 
// dick sites 2019.03.13, 2020.07.10
// These are from linux-4.19.19 x86 AMD 64-bit. Others will vary.
//

#ifndef __KUTRACE_CONTROL_NAMES_I3_H__
#define __KUTRACE_CONTROL_NAMES_I3_H__

#include "kutrace_lib.h"

// Get rid of this
static const char* CpuFamilyModelManuf = "23 17 AMD";

static const NumNamePair PidNames[] = {
  {0, "-idle-"},
  {-1, NULL},		// Must be last
};

static const NumNamePair Syscall32Names[] = {
  {1023, "-sched-"},	// Fake last syscall. Indicates where __schedule runs
  {-1, NULL},		// Must be last
};

// From linux-6.1.12/arch/x86/include/generated/uapi/asm/unistd_64.h
// grep define arch/x86/include/generated/uapi/asm/unistd_64.h |sed 's/#define __NR_\([^ ]*\) \([0-9]*\)/  \{\2, \"\1\"\},/'
static const NumNamePair Syscall64Names[] = {
  {0, "read"},
  {1, "write"},
  {2, "open"},
  {3, "close"},
  {4, "stat"},
  {5, "fstat"},
  {6, "lstat"},
  {7, "poll"},
  {8, "lseek"},
  {9, "mmap"},
  {10, "mprotect"},
  {11, "munmap"},
  {12, "brk"},
  {13, "rt_sigaction"},
  {14, "rt_sigprocmask"},
  {15, "rt_sigreturn"},
  {16, "ioctl"},
  {17, "pread64"},
  {18, "pwrite64"},
  {19, "readv"},
  {20, "writev"},
  {21, "access"},
  {22, "pipe"},
  {23, "select"},
  {24, "sched_yield"},
  {25, "mremap"},
  {26, "msync"},
  {27, "mincore"},
  {28, "madvise"},
  {29, "shmget"},
  {30, "shmat"},
  {31, "shmctl"},
  {32, "dup"},
  {33, "dup2"},
  {34, "pause"},
  {35, "nanosleep"},
  {36, "getitimer"},
  {37, "alarm"},
  {38, "setitimer"},
  {39, "getpid"},
  {40, "sendfile"},
  {41, "socket"},
  {42, "connect"},
  {43, "accept"},
  {44, "sendto"},
  {45, "recvfrom"},
  {46, "sendmsg"},
  {47, "recvmsg"},
  {48, "shutdown"},
  {49, "bind"},
  {50, "listen"},
  {51, "getsockname"},
  {52, "getpeername"},
  {53, "socketpair"},
  {54, "setsockopt"},
  {55, "getsockopt"},
  {56, "clone"},
  {57, "fork"},
  {58, "vfork"},
  {59, "execve"},
  {60, "exit"},
  {61, "wait4"},
  {62, "kill"},
  {63, "uname"},
  {64, "semget"},
  {65, "semop"},
  {66, "semctl"},
  {67, "shmdt"},
  {68, "msgget"},
  {69, "msgsnd"},
  {70, "msgrcv"},
  {71, "msgctl"},
  {72, "fcntl"},
  {73, "flock"},
  {74, "fsync"},
  {75, "fdatasync"},
  {76, "truncate"},
  {77, "ftruncate"},
  {78, "getdents"},
  {79, "getcwd"},
  {80, "chdir"},
  {81, "fchdir"},
  {82, "rename"},
  {83, "mkdir"},
  {84, "rmdir"},
  {85, "creat"},
  {86, "link"},
  {87, "unlink"},
  {88, "symlink"},
  {89, "readlink"},
  {90, "chmod"},
  {91, "fchmod"},
  {92, "chown"},
  {93, "fchown"},
  {94, "lchown"},
  {95, "umask"},
  {96, "gettimeofday"},
  {97, "getrlimit"},
  {98, "getrusage"},
  {99, "sysinfo"},
  {100, "times"},
  {101, "ptrace"},
  {102, "getuid"},
  {103, "syslog"},
  {104, "getgid"},
  {105, "setuid"},
  {106, "setgid"},
  {107, "geteuid"},
  {108, "getegid"},
  {109, "setpgid"},
  {110, "getppid"},
  {111, "getpgrp"},
  {112, "setsid"},
  {113, "setreuid"},
  {114, "setregid"},
  {115, "getgroups"},
  {116, "setgroups"},
  {117, "setresuid"},
  {118, "getresuid"},
  {119, "setresgid"},
  {120, "getresgid"},
  {121, "getpgid"},
  {122, "setfsuid"},
  {123, "setfsgid"},
  {124, "getsid"},
  {125, "capget"},
  {126, "capset"},
  {127, "rt_sigpending"},
  {128, "rt_sigtimedwait"},
  {129, "rt_sigqueueinfo"},
  {130, "rt_sigsuspend"},
  {131, "sigaltstack"},
  {132, "utime"},
  {133, "mknod"},
  {134, "uselib"},
  {135, "personality"},
  {136, "ustat"},
  {137, "statfs"},
  {138, "fstatfs"},
  {139, "sysfs"},
  {140, "getpriority"},
  {141, "setpriority"},
  {142, "sched_setparam"},
  {143, "sched_getparam"},
  {144, "sched_setscheduler"},
  {145, "sched_getscheduler"},
  {146, "sched_get_priority_max"},
  {147, "sched_get_priority_min"},
  {148, "sched_rr_get_interval"},
  {149, "mlock"},
  {150, "munlock"},
  {151, "mlockall"},
  {152, "munlockall"},
  {153, "vhangup"},
  {154, "modify_ldt"},
  {155, "pivot_root"},
  {156, "_sysctl"},
  {157, "prctl"},
  {158, "arch_prctl"},
  {159, "adjtimex"},
  {160, "setrlimit"},
  {161, "chroot"},
  {162, "sync"},
  {163, "acct"},
  {164, "settimeofday"},
  {165, "mount"},
  {166, "umount2"},
  {167, "swapon"},
  {168, "swapoff"},
  {169, "reboot"},
  {170, "sethostname"},
  {171, "setdomainname"},
  {172, "iopl"},
  {173, "ioperm"},
  {174, "create_module"},
  {175, "init_module"},
  {176, "delete_module"},
  {177, "get_kernel_syms"},
  {178, "query_module"},
  {179, "quotactl"},
  {180, "nfsservctl"},
  {181, "getpmsg"},
  {182, "putpmsg"},
  {183, "afs_syscall"},
  {184, "tuxcall"},
  {185, "security"},
  {186, "gettid"},
  {187, "readahead"},
  {188, "setxattr"},
  {189, "lsetxattr"},
  {190, "fsetxattr"},
  {191, "getxattr"},
  {192, "lgetxattr"},
  {193, "fgetxattr"},
  {194, "listxattr"},
  {195, "llistxattr"},
  {196, "flistxattr"},
  {197, "removexattr"},
  {198, "lremovexattr"},
  {199, "fremovexattr"},
  {200, "tkill"},
  {201, "time"},
  {202, "futex"},
  {203, "sched_setaffinity"},
  {204, "sched_getaffinity"},
  {205, "set_thread_area"},
  {206, "io_setup"},
  {207, "io_destroy"},
  {208, "io_getevents"},
  {209, "io_submit"},
  {210, "io_cancel"},
  {211, "get_thread_area"},
  {212, "lookup_dcookie"},
  {213, "epoll_create"},
  {214, "epoll_ctl_old"},
  {215, "epoll_wait_old"},
  {216, "remap_file_pages"},
  {217, "getdents64"},
  {218, "set_tid_address"},
  {219, "restart_syscall"},
  {220, "semtimedop"},
  {221, "fadvise64"},
  {222, "timer_create"},
  {223, "timer_settime"},
  {224, "timer_gettime"},
  {225, "timer_getoverrun"},
  {226, "timer_delete"},
  {227, "clock_settime"},
  {228, "clock_gettime"},
  {229, "clock_getres"},
  {230, "clock_nanosleep"},
  {231, "exit_group"},
  {232, "epoll_wait"},
  {233, "epoll_ctl"},
  {234, "tgkill"},
  {235, "utimes"},
  {236, "vserver"},
  {237, "mbind"},
  {238, "set_mempolicy"},
  {239, "get_mempolicy"},
  {240, "mq_open"},
  {241, "mq_unlink"},
  {242, "mq_timedsend"},
  {243, "mq_timedreceive"},
  {244, "mq_notify"},
  {245, "mq_getsetattr"},
  {246, "kexec_load"},
  {247, "waitid"},
  {248, "add_key"},
  {249, "request_key"},
  {250, "keyctl"},
  {251, "ioprio_set"},
  {252, "ioprio_get"},
  {253, "inotify_init"},
  {254, "inotify_add_watch"},
  {255, "inotify_rm_watch"},
  {256, "migrate_pages"},
  {257, "openat"},
  {258, "mkdirat"},
  {259, "mknodat"},
  {260, "fchownat"},
  {261, "futimesat"},
  {262, "newfstatat"},
  {263, "unlinkat"},
  {264, "renameat"},
  {265, "linkat"},
  {266, "symlinkat"},
  {267, "readlinkat"},
  {268, "fchmodat"},
  {269, "faccessat"},
  {270, "pselect6"},
  {271, "ppoll"},
  {272, "unshare"},
  {273, "set_robust_list"},
  {274, "get_robust_list"},
  {275, "splice"},
  {276, "tee"},
  {277, "sync_file_range"},
  {278, "vmsplice"},
  {279, "move_pages"},
  {280, "utimensat"},
  {281, "epoll_pwait"},
  {282, "signalfd"},
  {283, "timerfd_create"},
  {284, "eventfd"},
  {285, "fallocate"},
  {286, "timerfd_settime"},
  {287, "timerfd_gettime"},
  {288, "accept4"},
  {289, "signalfd4"},
  {290, "eventfd2"},
  {291, "epoll_create1"},
  {292, "dup3"},
  {293, "pipe2"},
  {294, "inotify_init1"},
  {295, "preadv"},
  {296, "pwritev"},
  {297, "rt_tgsigqueueinfo"},
  {298, "perf_event_open"},
  {299, "recvmmsg"},
  {300, "fanotify_init"},
  {301, "fanotify_mark"},
  {302, "prlimit64"},
  {303, "name_to_handle_at"},
  {304, "open_by_handle_at"},
  {305, "clock_adjtime"},
  {306, "syncfs"},
  {307, "sendmmsg"},
  {308, "setns"},
  {309, "getcpu"},
  {310, "process_vm_readv"},
  {311, "process_vm_writev"},
  {312, "kcmp"},
  {313, "finit_module"},
  {314, "sched_setattr"},
  {315, "sched_getattr"},
  {316, "renameat2"},
  {317, "seccomp"},
  {318, "getrandom"},
  {319, "memfd_create"},
  {320, "kexec_file_load"},
  {321, "bpf"},
  {322, "execveat"},
  {323, "userfaultfd"},
  {324, "membarrier"},
  {325, "mlock2"},

  {326, "copy_file_range"},
  {327, "preadv2"},
  {328, "pwritev2"},
  {329, "pkey_mprotect"},
  {330, "pkey_alloc"},
  {331, "pkey_free"},
  {332, "statx"},
  {333, "io_pgetevents"},
  {334, "rseq"},
  {424, "pidfd_send_signal"},
  {425, "io_uring_setup"},
  {426, "io_uring_enter"},
  {427, "io_uring_register"},
  {428, "open_tree"},
  {429, "move_mount"},
  {430, "fsopen"},
  {431, "fsconfig"},
  {432, "fsmount"},
  {433, "fspick"},
  {434, "pidfd_open"},
  {435, "clone3"},
  {436, "close_range"},
  {437, "openat2"},
  {438, "pidfd_getfd"},
  {439, "faccessat2"},
  {440, "process_madvise"},
  {441, "epoll_pwait2"},
  {442, "mount_setattr"},
  {443, "quotactl_fd"},
  {444, "landlock_create_ruleset"},
  {445, "landlock_add_rule"},
  {446, "landlock_restrict_self"},
  {447, "memfd_secret"},
  {448, "process_mrelease"},
  {449, "futex_waitv"},
  {450, "set_mempolicy_home_node"},
  {451, "syscalls"},

  {1023, "-sched-"},	// Fake last syscall. Indicates where __schedule runs
  {-1, NULL},		// Must be last
};

// Based on arch/x86/include/asm/x86/irq_vectors.h
//    2017: arch/x86/include/asm/irq_vectors.h
//    2019: arch/x86/include/asm/irq_vectors.h
/*
 *  Vectors   0 ...  31 : system traps and exceptions - hardcoded events
 *  Vectors  32 ... 127 : device interrupts
 *  Vector  128         : legacy int80 syscall interface
 *  Vectors 129 ... INVALIDATE_TLB_VECTOR_START-1 except 204 : device interrupts
 *  Vectors INVALIDATE_TLB_VECTOR_START ... 255 : special interrupts
 */

/***
~/linux-6.1.12$ cat /proc/interrupts
            CPU0       CPU1       CPU2       CPU3       
   0:         11          0          0          0  IR-IO-APIC    2-edge      timer
   8:          0          0          0          0  IR-IO-APIC    8-edge      rtc0
   9:          0          0          0          0  IR-IO-APIC    9-fasteoi   acpi
 120:          0          0          0          0  DMAR-MSI    0-edge      dmar0
 121:          0          0          0          0  DMAR-MSI    1-edge      dmar1
 125:          0         12          0          0  IR-PCI-MSI 524288-edge      nvme0q0
 126:          0          0         86          0  IR-PCI-MSI 327680-edge      xhci_hcd
 127:         97          0          0          0  IR-PCI-MSI 524289-edge      nvme0q1
 128:          0         25          0          0  IR-PCI-MSI 524290-edge      nvme0q2
 129:          0          0         89          0  IR-PCI-MSI 524291-edge      nvme0q3
 130:          0          0          0         19  IR-PCI-MSI 524292-edge      nvme0q4
 131:          0      47097       7148          0  IR-PCI-MSI 376832-edge      ahci[0000:00:17.0]
 132:         78          0          0     262704  IR-PCI-MSI 1048576-edge      enp2s0
 133:          0          0          0        463  IR-PCI-MSI 514048-edge      snd_hda_intel:card0
 NMI:          1          1          1          1   Non-maskable interrupts
 LOC:     380691     378255     378221     380382   Local timer interrupts
 SPU:          0          0          0          0   Spurious interrupts
 PMI:          1          1          1          1   Performance monitoring interrupts
 IWI:          0          0          0          0   IRQ work interrupts
 RTR:          0          0          0          0   APIC ICR read retries
 RES:        468        430        517        461   Rescheduling interrupts
 CAL:       5460       5010       4892       5592   Function call interrupts
 TLB:       2102       2009       1803       1715   TLB shootdowns
 TRM:          0          0          0          0   Thermal event interrupts
 THR:          0          0          0          0   Threshold APIC interrupts
 DFR:          0          0          0          0   Deferred Error APIC interrupts
 MCE:          0          0          0          0   Machine check exceptions
 MCP:          5          6          6          6   Machine check polls
 ERR:          0
 MIS:          0
 PIN:          0          0          0          0   Posted-interrupt notification event
 NPI:          0          0          0          0   Nested posted-interrupt event
 PIW:          0          0          0          0   Posted-interrupt wakeup event
***/

static const NumNamePair IrqNames[] = {
  // 2017 machines
  {0, "timer"},			// timer
  {1, "i8042_keyboard1"},	// keyboard/touchpad/mouse
  {3, "int3"},			// int3
  {8, "rtc0"},			// real-time clock chip
  {9, "acpi"},
  {12, "i8042_keyboard12"},	// keyboard/touchpad/mouse
  {16, "usb1"},
  {23, "usb2"},
  {24, "i915_graphics"},		// usb
  {28, "enp2s0_eth0"},		// aka eth0
  {29, "hda_29_inner"},		// disk
  {30, "hda_30_inner"},		// disk
  {31, "mei_me"},		// Management Engine Interface
  //{29, "snd_hda_29"},		// audio ?? maybe 32/33 now
  //{30, "snd_hda_30"},		// audio ??
  {36, "hda_36"},

  // 2017 for our particular machines dclab-1,2,3,4
  {0x22, "eth0"},		// eth0
  {0x42, "hda_29"},		// disk, forwarded to 29
  {0x52, "hda_30"},		// disk, forwarded to 30
  {0x62, "hda_31"},		// disk
  {0xb1, "graphics"},		// ether?, forwards to 24, no return

  // 2018 for our particular machines dclab-1,2,3
  {0xb2, "eth0"},		// ethernet


  {128, "int80"},

  {129, "eth0"},

// 2019.03.05 Linux 4.19 Ryzen */
  //{0x21, "??"},		// 1/sec
  {0x23, "eth0"},		// 18/sec
  //{0x24, "??"},		// 129 as 64+64 5 sec
  {0x25, "eth0"},		// aka eth0
  {0x27, "sdb2"},		// aka disk

 // {255, "spurious_apic"},
  {255, "BH"},			// bottom half of an interrupt handler
  {254, "error_apic_ipi"},
  {253, "reschedule_ipi"},
  {252, "call_func_ipi"},
  {251, "call_func1_ipi"},
  {250, "thermal_apic_ipi"},
  {249, "threshold_apic_ipi"},
  {248, "reboot_ipi"},
  {247, "x86_platform_ipi"},
  {246, "irq_work_ipi"},
  {245, "uv_bau_message"},
  {244, "deferred_error"},
  {243, "hypervisor_callback"},
  {242, "posted_intr"},
  {241, "posted_intr_wakeup"},
  {240, "posted_intr_nested"},
  {239, "managed_irq_shutdown"},
  {238, "hyperv_reenlighten"},
  {237, "hyperv_stimer0"},
  {236, "local_timer_vector"},	// event 0x05ec, decimal 1516 4.19 x86

  {13, "fpu_irq"},

  {-1, NULL},		// Must be last
};

// Export this to raw2event.cc, using above value
static const int kTIMER_IRQ_EVENT = 0x05ec;

// Bottom half BH vectors, from include/linux/interrupt.h
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

  {-1, NULL},		// Must be last
};

static const NumNamePair TrapNames[] = {
  {0, "Divide-by-zero"},
  {1, "Debug"},
  {2, "Non-maskable_Interrupt"},
  {3, "Breakpoint"},
  {4, "Overflow"},
  {5, "Bound_Range_Exceeded"},
  {6, "Invalid_Opcode"},
  {7, "device_not_available"},
  {8, "Double_Fault"},
  {9, "Coprocessor_Segment_Overrun"},
  {10, "Invalid_TSS"},
  {11, "Segment_Not_Present"},
  {12, "Stack_Segment_Fault"},
  {13, "General_Protection_Fault"},
  {14, "page_fault"},
  {15, "Spurious_Interrupt"},
  {16, "x87_Floating-Point_Exception"},
  {17, "Alignment_Check"},
  {18, "Machine_Check"},
  {19, "SIMD_Floating-Point_Exception"},
  {32, "IRET_Exception"},

  {-1, NULL},		// Must be last
};

// This is just the base set. More could be added later
// see linux-4.19.19/tools/include/uapi/asm-generic/errno-base.h
//  linux-4.19.19/include/linux/errno.h
//  linux-4.19.19/include/uapi/linux/errno.h
//  linux-4.19.19/include/uapi/asm-generic/errno.h

static const NumNamePair ErrnoNames[] = {
  {1, "EPERM"},
  {2, "ENOENT"},
  {3, "ESRCH"},
  {4, "EINTR"},
  {5, "EIO"},
  {6, "ENXIO"},
  {7, "E2BIG"},
  {8, "ENOEXEC"},
  {9, "EBADF"},
  {10, "ECHILD"},
  {11, "EAGAIN"},
  {12, "ENOMEM"},
  {13, "EACCES"},
  {14, "EFAULT"},
  {15, "ENOTBLK"},
  {16, "EBUSY"},
  {17, "EEXIST"},
  {18, "EXDEV"},
  {19, "ENODEV"},
  {20, "ENOTDIR"},
  {21, "EISDIR"},
  {22, "EINVAL"},
  {23, "ENFILE"},
  {24, "EMFILE"},
  {25, "ENOTTY"},
  {26, "ETXTBSY"},
  {27, "EFBIG"},
  {28, "ENOSPC"},
  {29, "ESPIPE"},
  {30, "EROFS"},
  {31, "EMLINK"},
  {32, "EPIPE"},
  {33, "EDOM"},
  {34, "ERANGE"},

  {-1, NULL},		// Must be last

};
#endif	// __KUTRACE_CONTROL_NAMES_I3_H__


