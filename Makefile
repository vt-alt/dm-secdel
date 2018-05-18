# SPDX-License-Identifier: GPL-2.0

KVER	?= $(shell uname -r)
KDIR	?= /lib/modules/$(KVER)/build/
DEPMOD	= /sbin/depmod -a
VERSION	= $(shell git -C $M describe --tag --dirty --always)
obj-m	= dm-linear.o
CFLAGS_dm-linear.o = -DDEBUG -DCONFIG_DM_DEBUG

all: dm-linear.ko

dm-linear.ko: dm-linear.c
	make -C $(KDIR) M=$(CURDIR) modules CONFIG_DEBUG_INFO=y

install: install-mod

install-mod: dm-linear.ko
	make -C $(KDIR) M=$(CURDIR) modules_install INSTALL_MOD_PATH=$(DESTDIR)

clean:
	-make -C $(KDIR) M=$(CURDIR) clean
	-rm -f *.so *.o modules.order

.PHONY: clean all install install-mod
