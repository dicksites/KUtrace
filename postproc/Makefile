ccflags-y := -march=native
CFLAGS_kutrace_mod.o = -march=native
#CFLAGS_kutrace_mod_llc.o = -march=native

obj-m += kutrace_mod.o
# obj-m += kutrace_mod_llc.o

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean


