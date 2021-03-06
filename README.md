dm-linear like target which provides discard, but replaces it with write of
erase pattern data to a discarded region. Thus, discarded data is securely
deleted (sanitized). Because of abstract nature, it could support many
file-systems which support discard (such as ext3, ext4, xfs, btrfs).

Operation notes:

Create a mapped device with `secdelsetup` tool. Make sure file-system is
mounted from that device and not from the underlying device. Make sure
file-system is mounted with `-o discard` option. Do not enable data journaling
(such as `-o data=journal` do not enable it).  Note, that when you `rm` files
discards will be sent (and, thus, erasing will performed) asynchronously, so,
to make sure data is already erased issue `sync` or remount file-system with
`-o sync` option before `rm`.  If you wish that filenames are wiped too, first,
make sure file-system is created completely without journaling (such as
`mkfs.ext4 -O ^has_journal`, and second, delete the directory itself, so its
blocks are discarded and erased. If you issue `fstrim` all free blocks of
file-system will be discarded and thus erased too (make sure that file-system
is still mounted with `-o discard` though.)

Usage:

```
secdelsetup /dev/sda5 [/dev/mapper/secdel5]
```
- will map `sda5` to `secdel5`. (With default erase more which is single pass of
(crng) random data). Alternatively:

```
secdelsetup /dev/sda5 [/dev/mapper/secdel5] 1R0
```
- Will work same as above but with with three pass overwriting: first pass of 1-bits,
second pass of (crng) random bits, and third pass of 0-bits.

Then, file-system on `secdel5` should be mounted with `-o discard`.

```
secdeltab --all or secdeltab --list
```
- show current maps.

```
secdelsetup --save
```
- save current maps to `/etc/secdeltab` which will be automatically activated
after reboot (by `secdeltab.service` systemd unit).

```
secdeltab --detach-all
```
- detach all active maps.

Based on the code of `dm-linear` from Linux kernel of their respective authors.
 (C) 2018,2019 <vt@altlinux.org>; License GPL-2.0-only.

