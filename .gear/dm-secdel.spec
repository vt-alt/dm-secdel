Name: dm-secdel
Version: 1.0.3
Release: alt1

Summary: dm-linear with secure deletion on discard
License: GPLv2
Group: System/Kernel and hardware
Requires: /sbin/dmsetup /sbin/blockdev /usr/bin/expr
BuildArch: noarch

Url: https://github.com/vt-alt/dm-secdel
Source0: %name-%version.tar

BuildRequires(pre): rpm-build-kernel

%description
Linear device-mapper target with secure deletion on discard.

%package -n kernel-source-%name
Summary: dm-linear with secure deletion on discard (source)
Group: Development/Kernel
BuildArch: noarch

%description -n kernel-source-%name
Linear device-mapper target with secure deletion on discard (source).

%prep
%setup -q

%build

%install
make install-bin DESTDIR=%buildroot
install -pDm0644 %_sourcedir/%name-%version.tar %kernel_srcdir/kernel-source-%name-%version.tar
mkdir %buildroot/etc
echo '# <target name> <source device> <options>' > %buildroot/etc/secdeltab

%files -n kernel-source-%name
%attr(0644,root,root) %kernel_src/kernel-source-%name-%version.tar

%files
%doc README.md
%config /etc/secdeltab
/sbin/secdelsetup
%{_unitdir}/secdeltab.service

%post
%post_service secdeltab
systemctl -q enable secdeltab

%preun
%preun_service secdeltab

%changelog
* Mon Jul 09 2018 Vitaly Chikunov <vt@altlinux.org> 1.0.3-alt1
- Compatibility with kernel 4.14

* Mon May 28 2018 Vitaly Chikunov <vt@altlinux.org> 1.0.2-alt1
- Proper install of secdel user space.

* Sun May 27 2018 Vitaly Chikunov <vt@altlinux.org> 1.0.1-alt1
- Systemd support.

* Thu May 24 2018 Vitaly Chikunov <vt@altlinux.org> 1.0.0-alt1
- Package for ALT.
