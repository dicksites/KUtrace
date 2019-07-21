# the compiler: gcc for C program, define as g++ for C++
  CC = g++

  # compiler flags:
  #  -g    adds debugging information to the executable file
  #  -Wall turns on most, but not all, compiler warnings
  CFLAGS  = -O2 
  # the build target executable:

  all: eventtospan rawtoevent spantospan spantotrim makeself
  eventtospan: eventtospan.cc
	$(CC) $(CFLAGS) -o eventtospan eventtospan.cc
  rawtoevent: rawtoevent.cc
	$(CC) $(CFLAGS) -o rawtoevent rawtoevent.cc from_base40.cc ../control/kutrace_lib.cc
  spantospan:
	$(CC) $(CFLAGS) -o spantospan spantospan.cc 
  spantotrim:
	$(CC) $(CFLAGS) -o spantotrim spantotrim.cc from_base40.cc
  makeself:
	$(CC) $(CFLAGS) -o makeself makeself.cc
  clean:
	rm -f *.o eventtospan rawtoevent spantospan spantotrim makeself
