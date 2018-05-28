dm-linear like target which provides discard, but replaces it with write of
random data to a discarded region. Thus, discarded data is securely deleted.
Because of abstract nature it could support many file-systems which support
discard (such as ext3, ext4, xfs, btrfs).

Usage:

```
secdelsetup /dev/sda5 [/dev/mapper/secdel5]
```
- will map `sda5` to `secdel5`. Then, file-system on `secdel5` should be
mounted with `-o discard`.

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
 (C) 2018 <vt@altlinux.org>; License GPLv2.

