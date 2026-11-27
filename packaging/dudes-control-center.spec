# SPDX-License-Identifier: GPL-2.0-only
# RPM package (1.48.0) for Fedora and similar distributions. Built from the
# files "make stage" puts into a folder; the CI builds it in a Fedora
# container:
#   make stage STAGE_DIR=$PWD/build-stage/root
#   rpmbuild -bb packaging/dudes-control-center.spec \
#     --define "_stagedir $PWD/build-stage/root" --define "pkgversion $(cat VERSION)"
# Same layout as the deb (programs in /usr/local); the install scripts call
# the shared /usr/local/libexec/dudes-control-center/pkg-setup.

%global debug_package %{nil}
# no %changelog here (CHANGELOG.md is the changelog): no build date from it
%global source_date_epoch_from_changelog 0
%global _build_id_links none

Name:           dudes-control-center
Version:        %{pkgversion}
Release:        1%{?dist}
Summary:        Fan, CPU and keyboard backlight control for Clevo and other laptops
License:        GPL-2.0-only AND GPL-2.0-or-later
URL:            https://github.com/dudewayne/Dudes-Control-Center
ExclusiveArch:  x86_64

Requires:       gtk3
Requires:       cjson
Requires:       librsvg2
Requires:       dkms
Requires:       polkit
Requires:       shadow-utils
Requires:       kmod
Recommends:     kernel-devel

%description
Dudes Control Center controls the fans through the Embedded Controller
(Clevo) or the kernel's hwmon interface, CPU power modes and frequency
limits, the battery charge limit and the keyboard backlight (per-key RGB on
ITE 829x keyboards, other keyboards through the LED class). Root daemons do
the hardware work; a GTK 3 GUI and command-line tools talk to them. The
ite_829x and dcc_fan kernel modules are built with DKMS.

%prep

%build

%install
cp -a %{_stagedir}/. %{buildroot}/
# every file of the package, and the folders that belong to it alone
cd %{buildroot}
find . \( -type f -o -type l \) | sed 's|^\.||' | sort > %{_builddir}/dcc-files.lst
for d in /usr/local/libexec/dudes-control-center /usr/share/dudes-control-center \
         /usr/share/doc/dudes-control-center /usr/src/ite_829x-%{version}; do
    find ".$d" -type d | sed 's|^\.|%dir |' >> %{_builddir}/dcc-files.lst
done

%files -f %{_builddir}/dcc-files.lst

%post
/usr/local/libexec/dudes-control-center/pkg-setup configure || :

%preun
# $1 = 0: removed, not upgraded
if [ "$1" -eq 0 ]; then
    /usr/local/libexec/dudes-control-center/pkg-setup remove || :
    /usr/local/libexec/dudes-control-center/pkg-setup purge-dkms || :
fi
