# SPDX-License-Identifier: GPL-2.0
#
# Makefile for kutrace
#

obj-$(CONFIG_KUTRACE) += kutrace_mod.o

KERNEL_SRC ?= /lib/modules/$(shell uname -r)/build
M ?= $(shell pwd)

KBUILD_OPTIONS += CONFIG_KUTRACE=m

modules modules_install clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(M) $(KBUILD_OPTIONS) W=1 $(@)

