# Build file for KUtrace control and postprocessing programs
# dsites 2022.06.11

c++ -O2 kutrace_control.cc kutrace_lib.cc -o kutrace_control

c++ -O2 rawtoevent.cc from_base40.cc kutrace_lib.cc -o rawtoevent
c++ -O2 eventtospan3.cc -o eventtospan3
c++ -O2 makeself.cc -o makeself

c++ -O2 spantospan.cc -o spantospan
c++ -O2 spantotrim.cc from_base40.cc -o spantotrim
