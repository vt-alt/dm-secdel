#!/bin/bash
# SPDX-License-Identifier: GPL-2.0

set -eu +f

PATH=$PATH:/sbin:/usr/sbin
sz=1048576
mnt=/mnt

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

declare -xi iter+=1
atexit() {
	mountpoint -q $mnt && umount $mnt
	dmsetup remove secdel >/dev/null 2>&1
	rmmod dm_secdel >/dev/null
	test "${lodev:-}" && losetup -d $lodev
	test -e loop.img && rm -f loop.img
	test -e marker-file.blk && rm -f marker-file.blk
}
trap atexit exit

sysctl kernel.printk=8
modprobe dm-mod
insmod ./dm-secdel 2>/dev/null || :

srcversion=$(modinfo ./dm-secdel.ko | awk '$1 ~ /srcversion/ {print $2}')
modsrcver=$(cat /sys/module/dm_secdel/srcversion)
if [ "$srcversion" != "$modsrcver" ]; then
       echo "ERROR: loaded module version is different from what we want to test"
       exit 1
fi

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
		dmesg | sed '/'$ldmesg'/,$!d;//d'
	fi
}

echo :: creating test disk
log dd if=/dev/zero of=loop.img bs=$sz count=100 status=none
lodev=$(losetup --show -f loop.img)
dmsetup create secdel --table "0 $(blockdev --getsz $lodev) secdel $lodev 0"
dev=/dev/mapper/secdel

lsblk

fail() {
	echo $RED"FAILURE $*"$NORM
	exit 1
}
fill_zero() {
	local fn=$mnt/$RANDOM.zero

	if ! dd if=/dev/zero of=$fn bs=$sz count=1 status=none 2>/dev/null; then
		rm -f $fn
		return 1
	fi
	c+=1
}

fill_marker() {
	local fn=$mnt/$RANDOM.mark

	if [ ! -s marker-file.blk ]; then
		# generate position dependent marker for debug
		sect=$(for ((i=0;i<256;i++)); do
			printf "%sDARTHSIDIUS%04x" $((i / 32)) $((i * 16))
		done)
		while true; do
			echo -n "$sect"
		done | head -c$sz > marker-file.blk
	fi
	c+=1
	if ! dd if=marker-file.blk of=$fn status=none 2>/dev/null; then
		return 1
	fi
}

fill_fs() {
	declare -i c=0

	while true; do
		fill_zero   || :
		fill_marker || break
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

	echo : verify absence of marker on device
	echo 1 > /proc/sys/vm/drop_caches
	# find marker in free spaces
	if grep -a -q DARTHSIDIUS $dev; then
		declare -i num
		num=$(grep -a -o .DARTHSIDIUS $dev | wc -l)\*16
		grep -a -o .DARTHSIDIUS $dev | sort | uniq -c
		echo : Byte offset of first/last match:
		grep -b -a -o .DARTHSIDIUS $dev | sed -n '1p;$p'
		fail "Marker is found on erased space after $op ($num bytes)"
	else
		echo $GREEN"GOOD Marker is not found after $op"$NORM
	fi
}

test_rm() {
	log mount -t $fs -o $opts $dev $mnt
	echo $YELLOW"SUBTEST rm"$NORM
	fill_fs
	rm -f $mnt/*.mark
	sync

	check_corruption
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
	umount $mnt
	check_device fstrim
}

testfs() {
	fs=$1
	opts=${2:-discard}

	echo $BYELLOW"TEST $fs"$NORM
	if ! type -p mkfs.$fs >/dev/null; then
		echo $YELLOW"SKIPPED: no mkfs.$fs"$NORM
		return
	fi
	dmesg_start
	wipefs -q -a $dev
	log mkfs.$fs -q $dev
	test_rm
	#test_fstrim

	dmesg_show | grep -v 'device-mapper: secdel DEBUG'
	if ! dmesg_show | grep -q 'DISCARD.*sectors'; then
		fail "No discards issued to device"
	fi
	if dmesg_show | egrep --color ' BUG:|Call Trace:|Kernel panic|Oops'; then
		fail "Kernel error"
	fi
}

testfs btrfs
testfs ext4
testfs ext3
testfs xfs

set +e
echo $BGREEN"SUCCESS ($iter)"$NORM
if [ "${1:-}" = loop ]; then
	atexit
	exec $0 "$@"
fi
