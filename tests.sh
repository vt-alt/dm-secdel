#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# vim: set sw=8:

set -eu +f

PATH=$PATH:/sbin:/usr/sbin
declare -i bs=$((1024*1024)) count=100
mnt=/mnt
cd $(dirname $0)

RED=$'\e[0;31m'
BRED=$'\e[1;31m'
GREEN=$'\e[0;32m'
BGREEN=$'\e[1;32m'
YELLOW=$'\e[0;33m'
BYELLOW=$'\e[1;33m'
CYAN=$'\e[0;36m'
NORM=$'\e[m'

log() {
	echo "  $*"
	eval "$@"
}

sysctl kernel.printk=7
if auditctl -s | grep -q 'enabled 0'; then
	echo : enable audit
	auditctl -e 1 >/dev/null
fi
declare -xi iter=0
declare -r img=$HOME/loop.img
atexit() {
	echo :: unload everything
	mountpoint -q $mnt && umount $mnt
	dmsetup remove secdel2 >/dev/null 2>&1
	rmmod dm_secdel 2>/dev/null
	test "${lodev:-}" && losetup -d $lodev
	test -e $img && rm -f $img
	test -e $HOME/marker-file.blk && rm -f $HOME/marker-file.blk
}
trap atexit exit

echo :: loading dm-secdel module
# try to unload previously loaded module (may fail)
lsmod | grep -q ^dm_secdel && rmmod dm_secdel 2>/dev/null || :
modprobe dm-mod 2>/dev/null || : # sometimes required dependency
insmod ./dm-secdel.ko || :

if [ ! -e /sys/module/dm_secdel/srcversion ]; then
	echo "ERROR: module is not loaded."
	exit 1
fi
srcversion=$(modinfo ./dm-secdel.ko | awk '$1 ~ /srcversion/ {print $2}')
modsrcver=$(cat /sys/module/dm_secdel/srcversion)
if [ "$srcversion" != "$modsrcver" ]; then
	echo "ERROR: loaded module version is different from what we want to test"
	exit 1
else
	echo ":: module loaded ok"
fi

if dmsetup targets | grep -q -w ^secdel; then
	echo :: dm target is visible
else
	dmsetup targets
	echo "ERROR: dm target is not available"
	exit 1
fi

# dmesg incremental logger
declare -x ldmesg
dmesg() {
	command dmesg "$@" | sed 's/^\[ \+/[/'
}
dmesg_start() {
	ldmesg=$(dmesg | tail -1 | awk '{print$1}')
	ldmesg=${ldmesg%]}
	ldmesg=${ldmesg#[}
}
dmesg_show() {
	if [ "$ldmesg" ]; then
		if dmesg | grep -q $ldmesg; then
			dmesg | sed '/'$ldmesg'/,$!d;//d'
		else
			dmesg
		fi
	fi
}

echo :: creating test disk of $((bs+count)) bytes
log dd if=/dev/zero of=$img bs=$bs count=$count status=none
lodev=$(losetup --show -f $img)
dmsetup create secdel2 --table "0 $(blockdev --getsz $lodev) secdel $lodev 0"
dev=/dev/mapper/secdel2
ls -lL $dev
dmsetup table
lsblk

fail() {
	echo $RED"FAILURE $*"$NORM
	exit 1
}

fill_zero() {
	local fn=$mnt/$RANDOM.zero

	if [ -e $fn ]; then return; fi
	# create file filled with zeros for integrity checks later
	if ! dd if=/dev/zero of=$fn bs=$bs count=1 status=none 2>/dev/null; then
		rm -f $fn
		return 1
	fi
	c+=1
}

create_marker_file() {
	# generate file with markers
	if [ ! -s $HOME/marker-file.blk ]; then
		echo : generate marker file
		# generate position dependent marker for debug
		sect=$(for ((i=0;i<256;i++)); do
			printf "%sDARTHSIDIUSf%03x" $((i / 32)) $((i * 16))
		done)
		while true; do
			echo -n "$sect"
		done | head -c$bs > $HOME/marker-file.blk
		echo : - done
	fi
}

fill_file() {
	local fn=$mnt/$RANDOM.mark

	if [ -e $fn ]; then return; fi
	create_marker_file
	# copy marked file into target filesystem
	if ! dd if=$HOME/marker-file.blk of=$fn status=none 2>/dev/null; then
		return 1
	fi
	c+=1
}

fill_dir() {
	local dn=$mnt/$RANDOM.mark

	if [ -e $dn ]; then return; fi
	# create directory filled with marked filenames
	mkdir $dn
	for ((i=0;i<32;i++)); do
		fn=$(printf "%sDARTHSIDIUSd%04x" $((i / 32)) $((i * 16)))
		touch $dn/$fn
	done
	c+=1
}

fill_fs() {
	declare -i c=0

	# fill filesystem with unmarked and marked files and directories
	# until first write error
	while true; do
		printf "%d\r" $c
		fill_zero   || break
		fill_dir    || break
		fill_file   || break
	done
	fill_zero   || :
	echo : Written $c test files
	sync
}

zeromib=d29751f2649b32ff572b5e0a9f541ea660a50f94ff0beedfb0b692b924cc8025
zeromeg=30e14955ebf1352266dc2ff8067e68104607e750abb9d3b36582b8af909fcb58
check_zeromeg() {
	local fn=$1

	# verify that correct data is not corrupted
	sum=$(sha256sum $fn)
	sum=${sum/ */}
	if [ "$sum" != $zeromeg ]; then
		sha256sum $fn
		fail "Invalid sha256 for $fn"
	fi
}

check_corruption() {
	echo : verify no corruption in files
	for fn in $mnt/*.zero; do
		check_zeromeg $fn
	done
	echo $GREEN"GOOD No corruption"$NORM
}

check_device() {
	local op=$1

	echo ": verify absence of marker on the device"
	#echo ":  drop caches"
	#echo 1 > /proc/sys/vm/drop_caches

	# find marker directly on device
	if grep -a -q DARTHSIDIUS $dev; then
		declare -i num
		num=$(grep -a -o .DARTHSIDIUS. $dev | wc -l)\*16
		grep -a -o .DARTHSIDIUS. $dev | sort | uniq -c
		echo : Byte offset of first/last match:
		grep -b -a -o .DARTHSIDIUS. $dev | sed -n '1p;$p'
		fail "Marker is found on erased space after $op ($num bytes)"
	else
		echo $GREEN"GOOD Marker is not found after $op"$NORM
	fi
}

test_rm() {
	log mount -t $fs -o discard $dev $mnt
	echo $YELLOW"SUBTEST rm"$NORM
	fill_fs
	echo ":  deleting marked data"
	rm -rf $mnt/*.mark
	echo ":  sync"
	sync

	check_corruption
	echo ":  umount"
	umount $mnt
	check_device rm
}

test_fstrim() {
	echo $YELLOW"SUBTEST fstrim"$NORM
	mount -t $fs -o nodiscard $dev $mnt # without discard
	rm -f $mnt/*.zero
	fill_fs
	rm -f $mnt/*.mark
	sync
	log fstrim -v $mnt
	sync

	check_corruption
	echo ":  umount"
	umount $mnt
	check_device fstrim
}

testfs() {
	fs=$1
	shift
	opts=$@

	echo $BYELLOW"TEST $fs"$NORM
	if ! type -p mkfs.$fs >/dev/null; then
		echo $YELLOW"SKIPPED: no mkfs.$fs"$NORM
		return
	fi
	if ! grep -q -w $fs /proc/filesystems; then
		echo $YELLOW"SKIPPED: no $fs"$NORM
		return
	fi
	dmesg_start
	wipefs -q -a $dev
	log mkfs.$fs -q $opts $dev
	expr $fs : ext >/dev/null && tune2fs -l $dev | grep 'Filesystem features'
	test_rm
	#test_fstrim

	echo : analyse dmesg
	dmesg_show | egrep -q -v 'device-mapper: secdel: (DISCARD|DEBUG)' || :
	if ! dmesg_show | grep -q 'DISCARD.*sectors'; then
		fail "No discards issued to device"
	else
		echo - $(dmesg_show | grep -c 'device-mapper: secdel: DISCARD') discards are detected
	fi
	if dmesg_show | egrep --color ' BUG:|Call Trace:|Kernel panic|Oops'; then
		fail "Kernel error"
	fi
	iter+=1
}

testfs ext4 -O ^has_journal

set +e
echo $BGREEN"SUCCESS ($iter)"$NORM
if [ "${1:-}" = loop ]; then
	atexit
	exec $0 "$@"
fi
