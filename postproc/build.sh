g++ -O2 base40.cc -o base40
g++ -O2 eventtospan3.cc -o eventtospan3
g++ -O2 hello_world_trace.c kutrace_lib.cc -o hello_world_trace
g++ -O2 kuod.cc -o kuod
g++ -O2 kutrace_control.cc kutrace_lib.cc -o kutrace_control
g++ -O2 kutrace_unittest.cc kutrace_lib.cc -o kutrace_unittest
g++ -O2 makeself.cc -o makeself
g++ -O2 rawtoevent.cc -Wno-format-overflow  from_base40.cc kutrace_lib.cc -o rawtoevent
g++ -O2 samptoname_k.cc -o samptoname_k
g++ -O2 samptoname_u.cc -o samptoname_u
g++ -O2 spantospan.cc -o spantospan
g++ -O2 spantoprof.cc -o spantoprof
g++ -O2 spantotrim.cc from_base40.cc -o spantotrim
g++ -O2 time_getpid.cc kutrace_lib.cc -o time_getpid
g++ -O2 unmakeself.cc -o unmakeself

