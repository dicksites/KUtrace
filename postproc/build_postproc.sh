# Build file for KUtrace postprocessing programs
# dsites 2022.08.17

c++ -O2 checktrace.cc -o checktrace
c++ -O2 eventtospan3.cc -o eventtospan3
c++ -O2 kuod.cc -o kuod
c++ -O2 makeself.cc -o makeself
c++ -O2 rawtoevent.cc from_base40.cc kutrace_lib.cc -o rawtoevent
c++ -O2 rawtoevent.cc from_base40.cc -o rawtoevent
c++ -O2 samptoname_k.cc -o samptoname_k
c++ -O2 samptoname_u.cc -o samptoname_u
c++ -O2 spantoprof.cc -o spantoprof
c++ -O2 spantospan.cc -o spantospan
c++ -O2 spantotrim.cc from_base40.cc -o spantotrim
c++ -O2 time_getpid.cc kutrace_lib.cc -o time_getpid
c++ -O2 unmakeself.cc -o unmakeself


