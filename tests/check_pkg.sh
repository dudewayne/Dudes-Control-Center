#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Checks an installed RPM or Arch package (1.48.0), run by the CI in a Fedora
# or Arch container right after installing it with its dependencies.
# Usage: tests/check_pkg.sh <version>
set -u
VER="$1"
fails=0
check() { if eval "$1"; then echo "PASS  $2"; else echo "FAIL  $2"; fails=$((fails + 1)); fi; }

for b in dcc-gui dcc-fan-cli dcc-cpu-cli dcc-kbd-cli dcc-fan-daemon dcc-cpu-daemon dcc-kbd-daemon dcc-hard-limit-enforce; do
    check "[ -x /usr/local/bin/$b ]" "program $b installed"
done
for b in dcc-gui dcc-fan-cli dcc-cpu-cli dcc-kbd-cli dcc-fan-daemon dcc-cpu-daemon dcc-kbd-daemon; do
    check "! ldd /usr/local/bin/$b | grep -q 'not found'" "$b finds all its libraries"
done
check "[ -x /usr/local/sbin/dcc-component ]" "dcc-component installed"
check "/usr/local/bin/dcc-fan-cli --help | grep -q -- '--report'" "dcc-fan-cli runs"
check "/usr/local/bin/dcc-fan-cli --report | grep -q '^Version: $VER\$'" "dcc-fan-cli --report shows version $VER"
check "/usr/local/bin/dcc-cpu-cli --help | grep -q -- '--battery-limit'" "dcc-cpu-cli runs"
check "/usr/local/bin/dcc-kbd-cli --help | grep -q -- '--brightness'" "dcc-kbd-cli runs"

PS=/usr/local/libexec/dudes-control-center/pkg-setup
check "[ -x $PS ] && sh -n $PS && grep -qx 'VERSION=$VER' $PS && ! grep -q '@[A-Z]*@' $PS" "pkg-setup installed with version and group filled in"
check "getent group dudes-control-center >/dev/null" "the install script created the group dudes-control-center"
for u in dcc-fan-daemon.service dcc-cpu-daemon.service dcc-kbd-daemon.service dcc-hard-limit-enforcer.service dcc-hard-limit-enforcer.timer; do
    check "[ -f /etc/systemd/system/$u ]" "unit $u installed"
done
check "grep -q 'ExecStart=/usr/local/bin/dcc-fan-daemon' /etc/systemd/system/dcc-fan-daemon.service" "units start the programs in /usr/local/bin"
check "[ -f /usr/src/ite_829x-$VER/dkms.conf ] && [ -f /usr/src/ite_829x-$VER/dcc_fan.c ] && [ -f /usr/src/ite_829x-$VER/ite_829x.c ]" "DKMS source of ite_829x and dcc_fan in /usr/src/ite_829x-$VER"
check "grep -q 'PACKAGE_VERSION=\"$VER\"' /usr/src/ite_829x-$VER/dkms.conf" "dkms.conf has version $VER"
check "command -v dkms >/dev/null" "dkms installed as a dependency"
check "[ \"\$(cat /usr/lib/modules-load.d/dudes-control-center.conf)\" = dcc_fan ]" "dcc_fan loaded at boot (modules-load.d)"
check "[ -f /usr/share/polkit-1/actions/org.dudes-control-center.component.policy ]" "polkit policy installed"
check "[ -f /usr/share/locale/de/LC_MESSAGES/dudes-control-center.mo ]" "translations installed"
check "[ -f /usr/share/applications/dudes-control-center.desktop ] && [ -f /usr/share/icons/hicolor/256x256/apps/dudes-control-center.png ]" "menu entry and icon installed"
check "[ -f /usr/share/dudes-control-center/layouts/de/keys.csv ]" "keyboard layouts installed"
check "[ -f /usr/share/doc/dudes-control-center/README.md ] && [ -f /usr/share/doc/dudes-control-center/LICENSE ]" "README and license for the Help tab"
check "[ -f /usr/local/share/man/man1/dcc-gui.1.gz ]" "man pages installed"
writable=$(find /usr/local/bin /usr/local/sbin /usr/local/libexec/dudes-control-center /etc/systemd/system/dcc-* \
                \( -perm -g+w -o -perm -o+w \) ! -type l 2>/dev/null | head -3)
check "[ -z \"\$writable\" ]" "no program or unit writable by group or others (found: ${writable:-none})"
notroot=$(find /usr/local/bin/dcc-* /usr/local/libexec/dudes-control-center /usr/src/ite_829x-$VER ! -user root 2>/dev/null | head -3)
check "[ -z \"\$notroot\" ]" "files belong to root (not: ${notroot:-none})"

echo
echo "$fails failure(s)"
[ "$fails" -eq 0 ]
