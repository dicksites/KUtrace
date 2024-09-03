g++ -O2 -pthread client4.cc dclab_log.cc dclab_rpc.cc kutrace_lib.cc -o client4
g++ -O2 dumplogfile4.cc dclab_log.cc -o dumplogfile4
g++ -O2 eventtospan3.cc -o eventtospan3
g++ -O2 flt_hog.cc kutrace_lib.cc -o flt_hog
g++ -O2 hello_world_trace.c kutrace_lib.cc -o hello_world_trace
g++ -O2 kutrace_control.cc kutrace_lib.cc -o kutrace_control
g++ -O2 makeself.cc -o makeself
g++ -O2 matrix.cc  kutrace_lib.cc  -o matrix_ku
g++ -O2 memhog_3.cc kutrace_lib.cc -o memhog3
g++ -O2 memhog_ram.cc kutrace_lib.cc -o memhog_ram
g++ -O2 mystery0.cc -o mystery0
g++ -O2 mystery1.cc -o mystery1
g++ -O2 mystery2.cc -o mystery2
g++ -O2 mystery3.cc -lrt -o mystery3_opt
g++ -O2 -pthread mystery23.cc  kutrace_lib.cc  -o mystery23
g++ -O2 mystery25.cc kutrace_lib.cc  -o mystery25
g++ -O2 -pthread mystery27.cc fancylock2.cc mutex2.cc kutrace_lib.cc dclab_log.cc -o mystery27
g++ -O2 -pthread mystery27a.cc fancylock2.cc mutex2.cc kutrace_lib.cc dclab_log.cc -o mystery27a
g++ -O2 paging_hog.cc kutrace_lib.cc -o paging_hog
g++ -O2 pcaptojson.cc dclab_log.cc dclab_rpc.cc kutrace_lib.cc -lpcap -o pcaptojson
g++ -O2 -pthread queuetest.cc dclab_log.cc dclab_rpc.cc kutrace_lib.cc -o queuetest
g++ -O2 rawtoevent.cc from_base40.cc kutrace_lib.cc -o rawtoevent
g++ -O2 samptoname_k.cc -o samptoname_k
g++ -O2 samptoname_u.cc -o samptoname_u
g++ -O2 -pthread schedtest.cc  kutrace_lib.cc  -o schedtest 
g++ -O2 -pthread server4.cc dclab_log.cc dclab_rpc.cc kutrace_lib.cc spinlock.cc -o server4
g++ -O2 -pthread server_disk.cc dclab_log.cc dclab_rpc.cc kutrace_lib.cc spinlock_fixed.cc -o server_disk
g++ -O2 -pthread server_mystery21.cc dclab_log.cc dclab_rpc.cc kutrace_lib.cc spinlock_fixed.cc -o server_mystery21
g++ -O2 spantospan.cc -o spantospan
g++ -O2 spantoprof.cc -o spantoprof
g++ -O2 spantotrim.cc from_base40.cc -o spantotrim
g++ -O2 timealign.cc dclab_log.cc dclab_rpc.cc kutrace_lib.cc -o timealign
g++ -O2 time_getpid.cc kutrace_lib.cc -o time_getpid
g++ -O2 unmakeself.cc -o unmakeself
g++ -O2 whetstone_ku.c kutrace_lib.cc -lm -o whetstone_ku 


