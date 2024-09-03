// Little program to use locks
// Copyright 2021 Richard L. Sites
//
//
// We would like to run for a few hundred msec total, with perhaps 1000-10000 work
// calls and 10-ish debug calls so maybe wait 20 msec, total run 250-300 msec
// 3 threads doing updates each doing about 3000 calls in 200 msec = 70 usec each 
// complain if over 100 usec
// debug to use at least 200 usec and 1 msec better
//
// compile with g++ -O2 -pthread mystery27a.cc fancylock2.cc mutex2.cc kutrace_lib.cc dclab_log.cc -o mystery27a
//
// Command-line options:
// -smallwork -nowork 	control how much fake work is done by worker-threads holding the locks
// -dash0 -dash1 -dash2 -dash3 	control which locking style is usedby dashbaord threads

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>		// nanosleep
#include <unistd.h>		// read()
//#include <netinet/in.h> 
//#include <netinet/ip.h> /* superset of previous */
//#include <netinet/tcp.h>
//#include <sys/socket.h> 
//#include <sys/types.h> 

#include <string>

#include "basetypes.h"
#include "dclab_log.h"		// for GetUsec()
#include "fancylock2.h"
#include "kutrace_lib.h"
#include "mutex2.h"
#include "polynomial.h"
#include "timecounters.h"

using std::string;

#define handle_error_en(en, msg) \
  do { errno = en; perror(msg); exit(EXIT_FAILURE); } while (0)

static const int MAX_ACCOUNTS = 100;

static const int EXTRA_DASHBOARD_USEC = 500;
static const int FAKEWORK_ITER = 140;
static const int WORKER_ITER = 10000;
static const int DASHBOARD_ITER = 50;


enum ActionType {
  Deposit = 0,
  Getcash,
  Debitcard,
  Balance
};

static const char* kActionName[4] = {"Deposit", "Getcash", "Debitcard", "Balance"};
static const char* kActionNameStart[4] = {"depo", "cash", "debit", "bal"};
static const char* kActionNameEnd[4] = {"/depo", "/cash", "/debit", "/bal"};

static const ActionType kActionFreq[16] = { 
  Deposit, Getcash, Getcash, Debitcard,
  Debitcard, Debitcard, Balance, Balance,
  Balance, Balance, Balance, Balance, 
  Balance, Balance, Balance, Balance, 
};

static const bool debugging = true;

typedef struct {
  int incr_count;
  int decr_count;
  double balance;
  double other;
} Xdata;

typedef struct {
  int type;
  int account;
  double amount;
  int fake_work_usec;
  int pad;
} Action;

// The shared database
typedef struct {
  FancyLock2* readerlock[4];	// Four locks, for subscripting by account# low bits
  //FancyLock2* writerlock[2];
  Xdata bankbalance;
  Xdata accounts[MAX_ACCOUNTS];
} Database;

typedef struct {	/* Used as argument to created threads */
  pthread_t thread_id;	/* ID returned by pthread_create() */
  int thread_num;	/* Application-defined thread # */
  Database* db;		/* Shared data */
} ThreadInfo;

typedef void DashProc(int whoami, Database* db);

//
// ---- Globals
//

// Readers here are mutually exclusive but quick
// We expect the reader lock to take no more than 50 usec to acquire, 90% of the time
DEFINE_FANCYLOCK2(global_readerlock, 50);

// Writers are mutually exclusive and may take a while
// We expect the writer lock to take no more than 100 usec to acquire, 90% of the time
//DEFINE_FANCYLOCK2(global_writerlock, 100);

// More locks for experiment in spreadng locking around based on low bits
// of account number.
DEFINE_FANCYLOCK2(global_readerlock2, 50);
DEFINE_FANCYLOCK2(global_readerlock3, 50);
DEFINE_FANCYLOCK2(global_readerlock4, 50);
//DEFINE_FANCYLOCK2(global_writerlock2, 100);


static bool alwaysfalse;	// Assigned in main

// Command-line parameters set these
static int workshift;		// 0 shift gives 0..255 usec delay 
static int workmul;		// 1 gives WORKER_ITER iterations,2 2x, etc.
static bool lockbal;		// If true, take out the lock for balance transactions
static int lockmod;		// Mask. 0 = single locks, 1 = multiple locks based on low bit of account#, 3=lo 2 bits, etc.
static int nocapture;		// If true, reduce lock capture by waiting between acq 
static DashProc* dashproc;	// One of several dashboard procs. default: lock for ~500 usec 

// ---- End globals

// Wait n msec
void WaitMsec(int msec) {
  if (msec == 0) {return;}
  struct timespec req;
  req.tv_sec = msec / 1000;
  req.tv_nsec = (msec % 1000) * 1000000;
  nanosleep(&req, NULL);
}

 
// Do some low-issue-rate bogus work for approximately N microseconds
void DoFakeWork(int usec) {
  double bogus = alwaysfalse ? 1.0 : 3.0;
  for (int i = 0; i < usec; ++i) {
    for (int n = 0; n < FAKEWORK_ITER; ++n) {
      bogus /= 0.999999999;
      bogus /= 1.000000001;
    }
  }
  if (alwaysfalse) {printf("%f", bogus);}	// Keep live
}

void dumpaction(FILE* f,int whoami,  const Action* action) {
  int64 now = GetUsec();
  fprintf(f, "%02lld.%06llds [%d] Action %s(%d) $%5.2f %dus\n",
    (now / 1000000) % 60, now % 1000000, whoami, 
    kActionName[action->type], action->account, action->amount, action->fake_work_usec); 
}

// Little <action, amount> generator called by multiple threads.
// each with its own random# base on the stack. global pointer to database
void MakeAction(Action* action, uint32* rand) {
  uint32 x;
  *rand = x = POLYSHIFT32(*rand);
  memset(action, 0, sizeof(Action));
  action->type = kActionFreq[x & 15];
  action->account = (x >> 4) % MAX_ACCOUNTS; 
  action->amount = ((x >> 8) & 0xFFFF) / 100.0;	// $0.00 to 655.35
  action->fake_work_usec = (x >> 24) & 0xFF;	// 0 to 255 usec
  action->fake_work_usec >>= workshift;
  switch (action->type) {
  case Deposit:
    break;
  case Getcash:
    // Multiple of $20.00, 2/5 of full range
    action->amount = - floor(action->amount / 50.00) * 20.00;	
    break;
  case Debitcard:
    // Multiple of $1, 1/5 of full range
    action->amount = - floor(action->amount / 5.00);
    break;
  case Balance:
    // No amount, half as much fake work
    action->amount = 0.0; 
    action->fake_work_usec >>= 1;
    break;
  default:
    break;
  }
}

void Update(Xdata* xdata, double amount) {
  if (amount >= 0.0) {
    ++xdata->incr_count;
  } else {
    ++xdata->decr_count;
  }
  xdata->balance += amount;
}

double ReadBalance(const Xdata* xdata) {
  return xdata->balance;
}

void DoAction(int whoami, const Action* action, Database* db) {
  double balance;
  int locknum = action->account & lockmod;
  switch (action->type) {
  case Deposit:
  case Getcash:
  case Debitcard:
    {
      // Take out both locks
      Mutex2 lock1(whoami, db->readerlock[locknum]);
      //Mutex2 lock2(whoami, db->writerlock[locknum]);
      Update(&db->accounts[action->account], action->amount);
      Update(&db->bankbalance, action->amount);
      DoFakeWork(action->fake_work_usec);
    }
    // Reduce odds of lock capture by delaying ~10 usec after freeing locks
    if (nocapture) {DoFakeWork(10);}
    break;
  case Balance:
    if (lockbal) {
      // Take out just the reader lock
      Mutex2 lock1(whoami, db->readerlock[locknum]);
      // Ignore the balance but make it live
      balance = ReadBalance(&db->accounts[action->account]);
      if (alwaysfalse) {printf("%f", balance);}
      DoFakeWork(action->fake_work_usec);
    } else {
      // No lock at all
      // Ignore the balance but make it live
      balance = ReadBalance(&db->accounts[action->account]);
      if (alwaysfalse) {printf("%f", balance);}
      DoFakeWork(action->fake_work_usec);
    }
    // Reduce odds of lock capture by delaying ~10 usec after freeing locks
    if (lockbal && nocapture) {DoFakeWork(10);}
    break;
  default:
    DoFakeWork(action->fake_work_usec);
    break;
  }
}

/***
dashboard output fake html
<html>
<body>
<pre>
  Account xxxx, incr xxxx, decr xxxx, balance xxxx
  Account xxxx, incr xxxx, decr xxxx, balance xxxx
  Bank total    incr xxxx, decr xxxx, balance xxxx
</pre>
</body>
</html>
***/

string BuildDashboardString(const Database* db) {
  char buffer[256];
  string s;
  s += "<html> <body> <pre>\n";
  s += "Dashboard\n";
  for (int i = 0; i < MAX_ACCOUNTS; ++i) {
    if (db->accounts[i].balance != 0.00) {
      sprintf(buffer, "account %04d deposits %4d, withdrawls %4d, balance %8.2f\n", 
        i, db->accounts[i].incr_count, db->accounts[i].decr_count, db->accounts[i].balance);
      s += buffer; 
    }
  }
  sprintf(buffer, "Bank Total   deposits %4d, withdrawls %4d, balance %8.2f\n", 
    db->bankbalance.incr_count, db->bankbalance.decr_count, db->bankbalance.balance);
  s += "</pre> </body> </html>\n";

  DoFakeWork(EXTRA_DASHBOARD_USEC);
  return s;
}

void NoLockDebugDashboard(int whoami, Database* db) {
  string s = BuildDashboardString(db);
  if (debugging) {
    fprintf(stdout, "%s\n", s.c_str());
  }
}
void DoDebugDashboard(int whoami, Database* db) {
  // Take out all locks
  Mutex2 lock1(whoami, db->readerlock[0]);
  Mutex2 lock2(whoami, db->readerlock[1]);
  Mutex2 lock3(whoami, db->readerlock[2]);
  Mutex2 lock4(whoami, db->readerlock[3]);
  //Mutex2 lock3(whoami, db->writerlock[0]);
  //Mutex2 lock4(whoami, db->writerlock[1]);
  string s = BuildDashboardString(db);
  if (debugging) {
    fprintf(stdout, "%s\n", s.c_str());
  }
}

void BetterDebugDashboard(int whoami, Database* db) {
  if (!debugging) {return;}

  {
    // Take out all locks
    Mutex2 lock1(whoami, db->readerlock[0]);
    Mutex2 lock2(whoami, db->readerlock[1]);
    Mutex2 lock3(whoami, db->readerlock[2]);
    Mutex2 lock4(whoami, db->readerlock[3]);

    //Mutex2 lock3(whoami, db->writerlock[0]);
    //Mutex2 lock4(whoami, db->writerlock[1]);
    string s = BuildDashboardString(db);
    fprintf(stdout, "%s\n", s.c_str());
  }
}

void EvenBetterDebugDashboard(int whoami, Database* db) {
  if (!debugging) {return;}
  Database db_copy;

  kutrace::mark_a("copy");
  {
    // Take out all locks
    Mutex2 lock1(whoami, db->readerlock[0]);
    Mutex2 lock2(whoami, db->readerlock[1]);
    Mutex2 lock3(whoami, db->readerlock[2]);
    Mutex2 lock4(whoami, db->readerlock[3]);
    //Mutex2 lock3(whoami, db->writerlock[0]);
    //Mutex2 lock4(whoami, db->writerlock[1]);
    db_copy = *db;
    // Free both locks on block exit
  }
  kutrace::mark_a("/copy");

  string s = BuildDashboardString(&db_copy);
  fprintf(stdout, "%s\n", s.c_str());
}


void DbInit(Database* db) {
  db->readerlock[0] = &global_readerlock;	// Multiple reader locks
  db->readerlock[1] = &global_readerlock2;
  db->readerlock[2] = &global_readerlock3;
  db->readerlock[3] = &global_readerlock4;
  //db->writerlock[0] = &global_writerlock;
  //db->writerlock[1] = &global_writerlock2;
  memset(&db->bankbalance, 0, sizeof(Xdata));
  memset(&db->accounts[0], 0, MAX_ACCOUNTS * sizeof(Xdata));
}

void* worker_thread(void* arg) {
  ThreadInfo* tinfo = (ThreadInfo*)(arg);
  int whoami = tinfo->thread_num;
  fprintf(stdout, "\nWorker thread %d started\n", whoami);
  Action action;
  uint32 rand = POLYINIT32;
  int count = WORKER_ITER * workmul;
  for (int i = 0; i < count; ++i) {
    MakeAction(&action, &rand);
////dumpaction(stdout, whoami, &action);
    kutrace::mark_a(kActionNameStart[action.type]);
    DoAction(whoami, &action, tinfo->db);
    kutrace::mark_a(kActionNameEnd[action.type]);
    // Indicate progress
    if (((i + 1) % 1000) == 0) {fprintf(stderr, "worker[%d] %4d\n", whoami, i+1);}
  }
  fprintf(stdout, "\nWorker thread %d finished\n", whoami);
  return NULL;
}

void* dashboard_thread(void* arg) {
  ThreadInfo* tinfo = (ThreadInfo*)(arg);
  int whoami = tinfo->thread_num;
  fprintf(stdout, "\nDashboard thread %d started\n", whoami);
  for (int i = 0; i < DASHBOARD_ITER; ++i) {
    WaitMsec(20);
    (*dashproc)(whoami, tinfo->db);
    // Indicate progress
    if (((i + 1) % 10) == 0) {fprintf(stderr, "dashboard[%d] %4d\n", whoami, i+1);}
  }
  fprintf(stdout, "\nDashboard thread %d finished\n", whoami);
  return NULL;
}

// Use this is to see if usec delay is in the ballpark.
// Early use might happen with a slow CPU clock, so also try later after CPUs are warmed up
void CheckFakeWork() {
  int64 start = GetUsec();
  DoFakeWork(1000);
  int64 elapsed = GetUsec() - start;
  fprintf(stdout,"DoFakeWork(1000) took %lld usec\n", elapsed);
}

void Usage() {
  fprintf(stderr, "Usage: mystery27 [-smallwork | -nowork] [-nolockbal] [-multilock] [-nocapture] [-dash0 | -dash1 | -dash2 | -dash3]\n");
  exit(0);
}

int main(int argc, const char** argv) {
  alwaysfalse = (time(NULL) == 0);	// Never true but the compiler doesn't know that
  Database db;
  DbInit(&db);
  CheckFakeWork();

  // Command-line parameters, if any
  workshift = 0;								// 0..255 usec delay
  workmul = 1;
  lockbal = true;
  lockmod = 0;
  nocapture = false;
  dashproc = &DoDebugDashboard;
  for (int i = 1; i < argc; ++i) {
    if (argv[i][0] != '-') {Usage();}
    if (strcmp(argv[i], "-smallwork") == 0) {workshift = 3; workmul = 1;}	// 0..15 usec delay
    else if (strcmp(argv[i], "-nowork") == 0) {workshift = 8; workmul = 2;}	// 0 usec delay
    else if (strcmp(argv[i], "-nolockbal") == 0) {lockbal = false;}		// No lock for balance transactions
    else if (strcmp(argv[i], "-multilock") == 0) {lockmod = 3;}			// Use multiple reader locks
    else if (strcmp(argv[i], "-nocapture") == 0) {nocapture = true;}		// Delay before re-acquire of locks
    else if (strcmp(argv[i], "-dash0") == 0) {dashproc = &NoLockDebugDashboard;}	// No dashboard locks at all
    else if (strcmp(argv[i], "-dash1") == 0) {dashproc = &DoDebugDashboard;}		// Long lock
    else if (strcmp(argv[i], "-dash2") == 0) {dashproc = &BetterDebugDashboard;}	// Early out long lock
    else if (strcmp(argv[i], "-dash3") == 0) {dashproc = &EvenBetterDebugDashboard;}	// Lock just copying
    else {Usage();}
  }

  // Launch several worker threads that update some shared data
  ThreadInfo tinfo[4];
  for (int tnum = 0; tnum < 3; tnum++) {
    tinfo[tnum].thread_num = tnum;
    tinfo[tnum].db = &db;
    int s = pthread_create(&tinfo[tnum].thread_id, NULL, &worker_thread, &tinfo[tnum]);
    if (s != 0) {handle_error_en(s, "pthread_create");}
  }

  // Launch a dashboard thread that reads the shared data
  for (int tnum = 3; tnum < 4; tnum++) {
    tinfo[tnum].thread_num = tnum;
    tinfo[tnum].db = &db;
    int s = pthread_create(&tinfo[tnum].thread_id, NULL, &dashboard_thread, &tinfo[tnum]);
    if (s != 0) {handle_error_en(s, "pthread_create");}
  }

  // Wait for all the threads to finish
  for (int tnum = 0; tnum < 4; tnum++) {
    int s = pthread_join(tinfo[tnum].thread_id, NULL);
    if (s != 0) {handle_error_en(s, "pthread_join");}
    CheckFakeWork();
  }
  fprintf(stderr, "All threads finished\n");

  return 0;
}





