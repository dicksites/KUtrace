# the compiler: gcc for C program, define as g++ for C++
  CC = gcc

  # compiler flags:
  #  -g    adds debugging information to the executable file
  #  -Wall turns on most, but not all, compiler warnings
  CFLAGS  = -O2 
  #DEPS = kutrace_lib.h
  # the build target executable:
  TARGET = kutrace_control

  all: $(TARGET)

  $(TARGET): $(TARGET).cc
	$(CC) $(CFLAGS) -o $(TARGET) $(TARGET).cc kutrace_lib.cc

  clean:
	$(RM) $(TARGET)
