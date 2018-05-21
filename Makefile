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

load: dm-linear.ko
	sysctl kernel.printk=8
	modprobe dm-mod
	insmod ./dm-linear.ko
	dmsetup create identity --table "0 `blockdev --getsz /dev/vda5` secdel /dev/vda5 0"
	mount -o discard /dev/mapper/identity /mnt

unload:
	-umount /mnt
	-dmsetup remove identity
	-rmmod dm-linear.ko

test:
	./tests.sh

.PHONY: clean all install install-mod
