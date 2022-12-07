ccflags-y := -march=rv64ima
CFLAGS_kutrace_mod.o = -march=rv64ima

obj-m += kutrace_mod.o

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean


