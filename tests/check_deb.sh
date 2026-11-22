#!/bin/bash
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

for b in dcc-gui dcc-fan-cli dcc-cpu-cli dcc-fan-daemon dcc-cpu-daemon dcc-kbd-daemon dcc-hard-limit-enforce; do
    check "[ -x '$R/usr/local/bin/$b' ] && [ ! -L '$R/usr/local/bin/$b' ]" "program $b installed"
done
for u in dcc-fan-daemon.service dcc-cpu-daemon.service dcc-kbd-daemon.service dcc-hard-limit-enforcer.service dcc-hard-limit-enforcer.timer; do
    check "[ -f '$R/etc/systemd/system/$u' ]" "unit $u installed"
done

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
check "grep -q 'TUXEDO Computers GmbH' '$R'/usr/src/ite_829x-*/ite_829x.c && grep -q 'GNU GENERAL PUBLIC LICENSE' '$R'/usr/src/ite_829x-*/LICENSE" "ite_829x source keeps the TUXEDO copyright and ships the GPL"
check "grep -q 'TUXEDO Computers GmbH' '$R/usr/share/doc/dudes-control-center/copyright'" "copyright file credits TUXEDO for the driver"

# maintainer scripts
for s in preinst postinst prerm postrm; do
    check "[ -x '$TMP/ctrl/$s' ] && sh -n '$TMP/ctrl/$s'" "$s present and valid shell"
    check "! grep -q '@[A-Z]*@' '$TMP/ctrl/$s'" "$s has no unfilled placeholders"
done
check "grep -q dudes-control-center-fan-daemon.service '$TMP/ctrl/preinst'" "preinst stops the old fan service before the upgrade"
check "grep -q 'wants/dudes-control-center-' '$TMP/ctrl/postinst'" "postinst removes leftovers of the old services"
check "grep -q 'systemctl enable dcc-fan-daemon' '$TMP/ctrl/postinst'" "postinst enables the new services"

echo
echo "$fails failure(s)"
exit $((fails > 0))
