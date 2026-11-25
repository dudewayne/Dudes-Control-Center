#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Checks the built .deb (run by "make deb-check" and CI after "make deb").
# Usage: tests/check_deb.sh <package.deb>
set -u
DEB="$1"
fails=0
check() { if eval "$1"; then echo "PASS  $2"; else echo "FAIL  $2"; fails=$((fails + 1)); fi; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
dpkg-deb -x "$DEB" "$TMP/root"
dpkg-deb -e "$DEB" "$TMP/ctrl"
R="$TMP/root"

for b in dcc-gui dcc-fan-cli dcc-cpu-cli dcc-kbd-cli dcc-fan-daemon dcc-cpu-daemon dcc-kbd-daemon dcc-hard-limit-enforce; do
    check "[ -x '$R/usr/local/bin/$b' ] && [ ! -L '$R/usr/local/bin/$b' ]" "program $b installed"
done
for u in dcc-fan-daemon.service dcc-cpu-daemon.service dcc-kbd-daemon.service dcc-hard-limit-enforcer.service dcc-hard-limit-enforcer.timer; do
    check "[ -f '$R/etc/systemd/system/$u' ]" "unit $u installed"
done

# ownership and permissions (1.24.1): everything root:root, nothing writable
# by group or others (the daemons run as root), programs executable
listing=$(dpkg-deb -c "$DEB"; dpkg-deb --ctrl-tarfile "$DEB" | tar -tvf -)
notroot=$(echo "$listing" | awk '$2 != "root/root" {print $NF}' | head -5)
check "[ -z \"\$notroot\" ]" "every file in the package belongs to root (not: ${notroot:-none})"
writable=$(echo "$listing" | awk 'substr($1,1,1) != "l" && (substr($1,6,1) == "w" || substr($1,9,1) == "w") {print $NF}' | head -5)
check "[ -z \"\$writable\" ]" "no file writable by group or others (found: ${writable:-none})"
for b in dcc-fan-daemon dcc-cpu-daemon dcc-kbd-daemon; do
    check "echo \"\$listing\" | grep -qE '^-rwxr-xr-x root/root .*usr/local/bin/$b\$'" "daemon $b is root-owned with mode 755"
done
check "grep -q 'dpkg-query -L dudes-control-center' '$TMP/ctrl/postinst' && grep -q 'chown root:root' '$TMP/ctrl/postinst'" "postinst gives files left by older packages back to root"
for c in preinst postinst prerm postrm; do
    check "echo \"\$listing\" | grep -qE '^-rwxr-xr-x root/root .*\\./$c\$'" "maintainer script $c is root-owned with mode 755"
done

# Keyboard layouts (1.28.0): key positions and picture per layout, SVG loader dependency
for c in de uk es jp kr; do
    check "[ -s '$R/usr/share/dudes-control-center/layouts/$c/keys.csv' ] && grep -q '<svg' '$R/usr/share/dudes-control-center/layouts/$c/keyboard_dark.svg' && grep -q '<svg' '$R/usr/share/dudes-control-center/layouts/$c/keyboard_light.svg'" "layout $c: keys.csv, dark and light picture installed"
done
check "grep -q '^Depends:.*librsvg2-common' '$TMP/ctrl/control'" "package depends on the SVG loader (librsvg2-common)"

for m in dark light; do
    check "[ -s '$R/usr/share/dudes-control-center/logo_$m.png' ]" "About page logo for $m mode installed"
done
# Application icon in four sizes (1.34.4); PNG width/height are bytes 16-23
pngsize() { od -An -tu1 -j16 -N8 "$1" | awk '{ printf "%d %d", $1*16777216+$2*65536+$3*256+$4, $5*16777216+$6*65536+$7*256+$8 }'; }
for n in 256 128 64 48; do
    f="$R/usr/share/icons/hicolor/${n}x${n}/apps/dudes-control-center.png"
    check "[ -s '$f' ] && [ \"\$(pngsize '$f')\" = '$n $n' ]" "application icon ${n}x${n} installed with the right size"
done

# Help tab (1.34.0): README, Changelog and License uncompressed in the doc folder
for f in README.md CHANGELOG.md LICENSE; do
    check "[ -s '$R/usr/share/doc/dudes-control-center/$f' ]" "Help tab file $f installed"
done
check "grep -q '^## \\[$(dpkg-deb -f "$DEB" Version)\\]' '$R/usr/share/doc/dudes-control-center/CHANGELOG.md'" "packaged Changelog has an entry for this version"

# Man pages (1.33.0): installed compressed, version filled in, render without warnings
for p in dudes-control-center.7 dcc-gui.1 dcc-fan-cli.1 dcc-cpu-cli.1 dcc-kbd-cli.1 dcc-fan-daemon.8 dcc-cpu-daemon.8 dcc-kbd-daemon.8 dcc-component.8; do
    f="$R/usr/local/share/man/man${p##*.}/$p.gz"
    check "[ -s '$f' ] && zcat '$f' | grep -q '^\\.TH .*\"Dudes Control Center [0-9][0-9.]*\"' && [ -z \"\$(zcat '$f' | groff -man -ww -z 2>&1)\" ]" "man page $p installed and renders without warnings"
done
for l in de es ja ko en_GB; do
    check "[ -s '$R/usr/share/locale/$l/LC_MESSAGES/dudes-control-center.mo' ]" "UI translation $l installed"
done

# Settings tab (1.27.0): root helper, polkit action, unit conditions
H="$R/usr/local/sbin/dcc-component"
check "echo \"\$listing\" | grep -qE '^-rwxr-xr-x root/root .*usr/local/sbin/dcc-component\$'" "helper dcc-component is root-owned with mode 755"
check "grep -q 'org.freedesktop.policykit.exec.path\">/usr/local/sbin/dcc-component<' '$R/usr/share/polkit-1/actions/org.dudes-control-center.component.policy' && grep -q '<allow_active>auth_admin_keep</allow_active>' '$R/usr/share/polkit-1/actions/org.dudes-control-center.component.policy'" "polkit action for dcc-component asks for the admin password"
for pair in fan:dcc-fan-daemon.service cpu:dcc-cpu-daemon.service cpu:dcc-hard-limit-enforcer.service cpu:dcc-hard-limit-enforcer.timer keyboard:dcc-kbd-daemon.service; do
    c=${pair%%:*}; u=${pair#*:}
    check "grep -q '^ConditionPathExists=!/etc/dudes-control-center/disabled-$c\$' '$R/etc/systemd/system/$u'" "$u does not start while $c is switched off"
done
check "sh '$H' bogus fan >/dev/null 2>&1; [ \$? -eq 2 ] && sh '$H' disable nothing >/dev/null 2>&1; [ \$? -eq 2 ]" "helper rejects unknown actions and components"
if [ "$(id -u)" -ne 0 ]; then
    check "sh '$H' disable fan >/dev/null 2>&1; [ \$? -eq 1 ]" "helper refuses to change anything without root"
fi
check "sh '$H' status | grep -qE '^fan (enabled|disabled)\$'" "helper status lists the components"

# every ExecStart/ExecStopPost and the desktop Exec point to a program in the package
for f in "$R"/etc/systemd/system/*.service "$R"/usr/share/applications/*.desktop; do
    for p in $(grep -hoE '^(ExecStart|ExecStopPost|Exec)=-?/[^ ]+' "$f" | sed -E 's/^[^=]+=-?//'); do
        check "[ -e '$R$p' ]" "$(basename "$f") runs $p, which is in the package"
    done
done

# old names (before 1.19.0): only compatibility links for the GUI and CLIs
old=$(cd "$R" && find . -name 'dudes-control-center-*' ! -type l | grep -vE '^\./usr/share/(doc|icons)' || true)
check "[ -z \"\$old\" ]" "no files with the old dudes-control-center-* names (found: ${old:-none})"
for n in gui fan-cli cpu-cli; do
    check "[ \"\$(readlink '$R/usr/local/bin/dudes-control-center-$n')\" = dcc-$n ]" "old command dudes-control-center-$n links to dcc-$n"
done

# third-party driver: TUXEDO copyright and GPL text shipped with it
# dcc_fan module (1.36.0): source, built with ite_829x, loaded at boot, own copyright entry
check "[ -s \"\$(ls '$R'/usr/src/ite_829x-*/dcc_fan.c)\" ] && grep -q '^obj-m += dcc_fan.o' '$R'/usr/src/ite_829x-*/Kbuild && grep -q 'DCC_FAN_VERSION' '$R'/usr/src/ite_829x-*/Kbuild" "dcc_fan source and Kbuild entry in the DKMS folder"
check "grep -q 'BUILT_MODULE_NAME\[1\]=\"dcc_fan\"' '$R'/usr/src/ite_829x-*/dkms.conf" "DKMS builds dcc_fan"
check "[ \"\$(cat '$R/usr/lib/modules-load.d/dudes-control-center.conf')\" = dcc_fan ]" "dcc_fan loaded at boot (modules-load.d)"
check "grep -q 'modprobe dcc_fan' '$TMP/ctrl/postinst' && grep -q 'rmmod dcc_fan' '$TMP/ctrl/prerm'" "postinst loads dcc_fan, prerm unloads it"
check "grep -q '^Files: usr/src/ite_829x-\*/dcc_fan.c' '$R/usr/share/doc/dudes-control-center/copyright'" "copyright file lists dcc_fan as project code"
check "grep -q 'TUXEDO Computers GmbH' '$R'/usr/src/ite_829x-*/ite_829x.c && grep -q 'GNU GENERAL PUBLIC LICENSE' '$R'/usr/src/ite_829x-*/LICENSE" "ite_829x source keeps the TUXEDO copyright and ships the GPL"
check "grep -q 'TUXEDO Computers GmbH' '$R/usr/share/doc/dudes-control-center/copyright'" "copyright file credits TUXEDO for the driver"
check "grep -q '^License: GPL-2.0-only' '$R/usr/share/doc/dudes-control-center/copyright' && grep -q 'agramian/clevo-fan-control' '$R/usr/share/doc/dudes-control-center/copyright'" "copyright file: project GPL-2.0-only, fan code origin credited"
check "grep -q 'GNU GENERAL PUBLIC LICENSE' '$R/usr/share/doc/dudes-control-center/LICENSE'" "project LICENSE with the GPL text installed"

# maintainer scripts
for s in preinst postinst prerm postrm; do
    check "[ -x '$TMP/ctrl/$s' ] && sh -n '$TMP/ctrl/$s'" "$s present and valid shell"
    check "! grep -q '@[A-Z]*@' '$TMP/ctrl/$s'" "$s has no unfilled placeholders"
done
check "grep -q dudes-control-center-fan-daemon.service '$TMP/ctrl/preinst'" "preinst stops the old fan service before the upgrade"
check "grep -q 'wants/dudes-control-center-' '$TMP/ctrl/postinst'" "postinst removes leftovers of the old services"
check "grep -q 'systemctl enable dcc-fan-daemon' '$TMP/ctrl/postinst'" "postinst enables the new services"
check "grep -q 'try-restart upower' '$TMP/ctrl/postinst'" "postinst lets UPower see the reloaded keyboard driver"

echo
echo "$fails failure(s)"
exit $((fails > 0))
