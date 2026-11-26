#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# dcc-component - switch a Dudes Control Center component on or off (1.27.0)
#
#   dcc-component disable fan|cpu|keyboard
#   dcc-component enable  fan|cpu|keyboard
#   dcc-component status
#   dcc-component ec-direct enable|disable   (1.41.0)
#
# Runs as root (the GUI starts it through pkexec; polkit asks for the
# administrator password). A disabled component has the flag file
# /etc/dudes-control-center/disabled-<component>; its units carry
# ConditionPathExists=! on that file, so systemd does not start them at boot
# and the package's postinst does not start them on an update either.
# Fan control switched off: the fan daemon's ExecStopPost fail-safe sets the
# fans to 100% (no EC command is known that hands them back to the
# firmware); after the next reboot the firmware controls them.
#
# ec-direct: direct EC access through the I/O ports 0x62/0x66 for laptops
# without the dcc_fan module (no Clevo WMI interface). Off by default: on a
# laptop with another EC layout, these writes can hit registers for charging,
# power or thermal limits and damage the hardware. Enabled, the file
# /etc/dudes-control-center/ec-direct-access contains "enabled"; the fan
# daemon still refuses the ports if the EC does not answer like a Clevo EC.
set -eu
PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH
DIR=/etc/dudes-control-center

usage() {
    echo "Usage: dcc-component enable|disable fan|cpu|keyboard  |  dcc-component ec-direct enable|disable  |  dcc-component status" >&2
    exit 2
}

if [ "${1:-}" = "status" ] && [ $# -eq 1 ]; then
    for c in fan cpu keyboard; do
        if [ -e "$DIR/disabled-$c" ]; then echo "$c disabled"; else echo "$c enabled"; fi
    done
    if [ "$(head -n 1 "$DIR/ec-direct-access" 2>/dev/null)" = "enabled" ]; then
        echo "ec-direct enabled"
    else
        echo "ec-direct disabled"
    fi
    exit 0
fi

if [ "${1:-}" = "ec-direct" ] && [ $# -eq 2 ]; then
    case "$2" in enable|disable) ;; *) usage ;; esac
    if [ "$(id -u)" -ne 0 ]; then
        echo "dcc-component: must run as root (the GUI uses pkexec)" >&2
        exit 1
    fi
    mkdir -p "$DIR"
    chmod 755 "$DIR"
    if [ "$2" = "enable" ]; then
        tmp="$DIR/.ec-direct-access.$$"
        echo enabled > "$tmp"
        chmod 644 "$tmp"
        mv -f "$tmp" "$DIR/ec-direct-access"
        msg="direct EC access enabled (unsupported hardware: at your own risk)"
    else
        rm -f "$DIR/ec-direct-access"
        msg="direct EC access disabled"
    fi
    # the fan daemon decides at start: restart it unless fan control is off
    if [ ! -e "$DIR/disabled-fan" ]; then
        systemctl reset-failed dcc-fan-daemon.service 2>/dev/null || true
        systemctl restart dcc-fan-daemon.service || true
    fi
    echo "$msg"
    logger -t dcc-component "$msg" 2>/dev/null || true
    exit 0
fi
[ $# -eq 2 ] || usage
case "$1" in enable|disable) ;; *) usage ;; esac

case "$2" in
    fan)      stop_units="dcc-fan-daemon.service";        start_units="dcc-fan-daemon.service" ;;
    cpu)      stop_units="dcc-hard-limit-enforcer.timer dcc-hard-limit-enforcer.service dcc-cpu-daemon.service"
              start_units="dcc-cpu-daemon.service dcc-hard-limit-enforcer.timer" ;;
    keyboard) stop_units="dcc-kbd-daemon.service";        start_units="dcc-kbd-daemon.service" ;;
    *) usage ;;
esac

if [ "$(id -u)" -ne 0 ]; then
    echo "dcc-component: must run as root (the GUI uses pkexec)" >&2
    exit 1
fi

case "$1" in
    disable)
        mkdir -p "$DIR"
        chmod 755 "$DIR"
        : > "$DIR/disabled-$2"
        chmod 644 "$DIR/disabled-$2"
        # shellcheck disable=SC2086
        systemctl stop $stop_units
        echo "$2 control disabled"
        logger -t dcc-component "$2 control disabled" 2>/dev/null || true
        ;;
    enable)
        rm -f "$DIR/disabled-$2"
        # shellcheck disable=SC2086
        systemctl reset-failed $start_units 2>/dev/null || true
        # shellcheck disable=SC2086
        systemctl start $start_units
        echo "$2 control enabled"
        logger -t dcc-component "$2 control enabled" 2>/dev/null || true
        ;;
    *) usage ;;
esac
