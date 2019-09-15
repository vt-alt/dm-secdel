# SPDX-License-Identifier: GPL-2.0-only

KVER	?= $(shell uname -r)
KDIR	?= /lib/modules/$(KVER)/build/
DEPMOD	= /sbin/depmod -a
VERSION	= $(shell git -C $M describe --tag --dirty --always)
obj-m	= dm-secdel.o
CFLAGS_dm-secdel.o = -DDEBUG -DCONFIG_DM_DEBUG

all: dm-secdel.ko

dm-secdel.ko: dm-secdel.c
	make -C $(KDIR) M=$(CURDIR) modules CONFIG_DEBUG_INFO=y

install: install-mod install-bin

install-mod: dm-secdel.ko
	make -C $(KDIR) M=$(CURDIR) modules_install INSTALL_MOD_PATH=$(DESTDIR)

install-bin: secdelsetup
	install -pD secdelsetup $(DESTDIR)/sbin/secdelsetup
	install -pD -m644 secdeltab.service $(DESTDIR)/lib/systemd/system/secdeltab.service
	-systemctl daemon-reload
	-systemctl enable secdeltab

uninstall:
	-rm -f $(DESTDIR)/sbin/secdelsetup
	-rm -f $(DESTDIR)/lib/modules/$(KVER)/extra/dm-secdel.ko
	-systemctl --no-reload disable --now secdeltab.service
	-rm -f $(DESTDIR)/lib/systemd/system/secdeltab.service
	-systemctl daemon-reload

clean:
	-make -C $(KDIR) M=$(CURDIR) clean
	-rm -f *.so *.o modules.order

load: dm-*.ko
	sysctl kernel.printk=8
	modprobe dm-mod
	insmod ./dm-secdel.ko
	dmsetup create identity --table "0 `blockdev --getsz /dev/vda5` secdel /dev/vda5 0"
	mount -o discard /dev/mapper/identity /mnt

unload:
	-umount /mnt
	-dmsetup remove identity
	-rmmod dm-secdel.ko

test:
	./tests.sh

.PHONY: clean all install install-mod install-bin uninstall load unload test
