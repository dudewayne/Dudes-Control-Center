# SPDX-License-Identifier: GPL-2.0-only
"""System report (1.45.0): dcc-fan-cli --report against a fake /sys, /proc
and /etc ("root") and fake daemon sockets.

Usage: python3 tests/test_report.py <empty-work-dir> <report_test binary>

Checks what the report shows (model, modules, opt-ins, daemon STATUS,
hwmon fans, keyboard LEDs, platform_profile, GPUs, power supplies) and that
it never contains the user name, the home folder or the host name.
"""
import getpass
import os
import socket
import subprocess
import sys
import threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fakesys import Checker, wr  # noqa: E402

base, binary = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
os.makedirs(base, exist_ok=True)
os.chdir(base)
check = Checker()
user = getpass.getuser()
home = os.path.expanduser("~")
host = socket.gethostname()

R = "root"
# DMI: the board name carries the host name, the BIOS vendor the user name,
# and a serial number that must never be read
wr(f"{R}/sys/class/dmi/id/sys_vendor", "TestVendor\n")
wr(f"{R}/sys/class/dmi/id/product_name", "Laptop 15\n")
wr(f"{R}/sys/class/dmi/id/board_vendor", "TestVendor\n")
wr(f"{R}/sys/class/dmi/id/board_name", f"{host}\n")
wr(f"{R}/sys/class/dmi/id/bios_version", "1.2.3\n")
wr(f"{R}/sys/class/dmi/id/bios_date", "01/02/2026\n")
wr(f"{R}/sys/class/dmi/id/bios_vendor", f"{user} BIOS\n")
wr(f"{R}/sys/class/dmi/id/product_serial", "SERIAL-123456\n")
wr(f"{R}/sys/class/dmi/id/product_uuid", "UUID-abcdef\n")
wr(f"{R}/etc/os-release", f'NAME="Test"\nPRETTY_NAME="TestOS 1.0 ({home}/x)"\n')
wr(f"{R}/proc/cpuinfo", "processor\t: 0\nmodel name\t: Test CPU 9000\nprocessor\t: 1\nmodel name\t: Test CPU 9000\n")
wr(f"{R}/sys/module/k10temp/refcnt", "0\n")
wr(f"{R}/sys/module/dcc_fan/version", "9.9.9\n")
wr(f"{R}/etc/dudes-control-center/hwmon-fans", "enabled\n")
wr(f"{R}/etc/dudes-control-center/disabled-keyboard", "")
wr(f"{R}/etc/dudes-control-center/user", f"{user}\n")
# hwmon: a CPU sensor, a fan device with a manual mode
wr(f"{R}/sys/class/hwmon/hwmon0/name", "k10temp\n")
wr(f"{R}/sys/class/hwmon/hwmon0/temp1_input", "50000\n")
wr(f"{R}/sys/class/hwmon/hwmon0/temp3_input", "51000\n")
wr(f"{R}/sys/class/hwmon/hwmon10/name", "thinkpad\n")
wr(f"{R}/sys/class/hwmon/hwmon10/fan1_input", "2900\n")
wr(f"{R}/sys/class/hwmon/hwmon10/pwm1", "128\n")
wr(f"{R}/sys/class/hwmon/hwmon10/pwm1_enable", "2\n")
os.makedirs(f"{R}/sys/bus/platform/drivers/thinkpad_hwmon", exist_ok=True)
os.makedirs(f"{R}/sys/class/hwmon/hwmon10/device", exist_ok=True)
os.symlink(os.path.abspath(f"{R}/sys/bus/platform/drivers/thinkpad_hwmon"), f"{R}/sys/class/hwmon/hwmon10/device/driver")
wr(f"{R}/sys/class/thermal/thermal_zone0/type", "acpitz\n")
# keyboard LEDs: a single-color one and a per-key set
wr(f"{R}/sys/class/leds/tpacpi::kbd_backlight/brightness", "1\n")
wr(f"{R}/sys/class/leds/tpacpi::kbd_backlight/max_brightness", "2\n")
for n in ["rgb:kbd_backlight"] + [f"rgb:kbd_backlight_{i}" for i in range(1, 5)]:
    wr(f"{R}/sys/class/leds/{n}/multi_intensity", "0 0 0\n")
    wr(f"{R}/sys/class/leds/{n}/multi_index", "red green blue\n")
    wr(f"{R}/sys/class/leds/{n}/max_brightness", "10\n")
    wr(f"{R}/sys/class/leds/{n}/brightness", "10\n")
wr(f"{R}/sys/class/leds/input3::capslock/brightness", "0\n")
wr(f"{R}/sys/firmware/acpi/platform_profile", "balanced\n")
wr(f"{R}/sys/firmware/acpi/platform_profile_choices", "low-power balanced performance\n")
wr(f"{R}/sys/class/drm/card0/device/vendor", "0x1002\n")
wr(f"{R}/sys/class/drm/card0/device/device", "0x73df\n")
wr(f"{R}/sys/class/drm/card0/device/boot_vga", "0\n")
wr(f"{R}/sys/class/drm/card0/device/power/runtime_status", "suspended\n")
wr(f"{R}/sys/class/drm/card0-eDP-1/status", "connected\n")
wr(f"{R}/sys/class/power_supply/BAT0/type", "Battery\n")
wr(f"{R}/sys/class/power_supply/BAT0/charge_control_end_threshold", "80\n")


# fake daemons: fan and keyboard answer STATUS, the CPU socket is missing
def serve(path, reply):
    if os.path.exists(path):
        os.unlink(path)
    srv = socket.socket(socket.AF_UNIX)
    srv.bind(path)
    srv.listen(4)

    def loop():
        while True:
            try:
                c, _ = srv.accept()
            except OSError:
                return
            c.recv(256)
            c.sendall(reply.encode())
            c.close()
    threading.Thread(target=loop, daemon=True).start()
    return srv


fan = serve("fan.sock", "STATUS OK\nEC_ACCESS=hwmon\nHWMON_FANS=enabled\nHWMON_FANS_DRIVER=thinkpad\n"
            f"CONFIG_NOTE={home}/.config\n")
kbd = serve("kbd.sock", "STATUS OK\nLED_COUNT=1\nKBD_TYPE=white\nKBD_DRIVER=thinkpad_acpi\n"
            + "".join(f"CPU_{i}_FREQ={1000 + i} MHz\n" for i in range(4))
            + "".join(f"CPU_{i}_GOVERNOR=powersave\n" for i in range(4)))

r = subprocess.run([binary, "--report"], capture_output=True, text=True, timeout=20,
                   env=dict(os.environ, HOME=home))
out = r.stdout
open("report.txt", "w").write(out)
check(r.returncode == 0 and out.startswith("Dudes Control Center system report"), "--report prints the report (no root needed)")
check("Version: 9.9.9" in out and "Model: TestVendor Laptop 15" in out and "BIOS: 1.2.3 (01/02/2026" in out,
      "program version, model and BIOS")
check("Distribution: TestOS 1.0" in out and "CPU: Test CPU 9000, 2 threads" in out, "distribution and CPU")
check("dcc_fan: loaded, version 9.9.9" in out and "k10temp: loaded" in out and "ite_829x" not in out,
      "loaded kernel modules only")
check("Switched off: keyboard" in out and "Generic fan control: enabled" in out and "Direct EC access: off" in out,
      "components switched off and the opt-ins")
check("EC_ACCESS=hwmon" in out and "HWMON_FANS_DRIVER=thinkpad" in out and "KBD_TYPE=white" in out,
      "the daemons' STATUS")
check("== CPU daemon (STATUS) ==\nnot reachable" in out, "a daemon that is not running: not reachable")
check("CPU_n_FREQ: 4 per-core values left out" in out and "CPU_n_GOVERNOR=powersave (all 4 cores)" in out
      and "CPU_2_FREQ" not in out, "per-core lines summed up")
check("hwmon0: k10temp - temp 2, fan 0" in out and
      "hwmon10: thinkpad, driver thinkpad_hwmon - temp 0, fan 1, pwm1 (enable 2)" in out,
      "hwmon devices with sensors, fans and pwm modes")
check("tpacpi::kbd_backlight: single color, brightness 1/2" in out and
      "rgb:kbd_backlight: multicolor, brightness 10/10, multi_index red green blue" in out and
      "plus 4 numbered LEDs" in out and "capslock" not in out, "keyboard LEDs")
check("platform_profile: balanced (choices: low-power balanced performance)" in out, "firmware power profile")
check("card0: driver ?, id 0x1002:0x73df, boot_vga 0, power suspended" in out and "card0-eDP-1" not in out,
      "graphics cards without connectors")
check("BAT0: Battery, charge_control_end_threshold" in out, "power supplies")
check("SERIAL-123456" not in out and "UUID-abcdef" not in out, "no serial number or UUID")
check(home not in out and "<home>/.config" in out and "<home>/x" in out, "the home folder is replaced")
check(f"{user} BIOS" not in out and "<user> BIOS" in out, f"the user name is replaced ({len(user)} characters)")
check("<host>" in out and f"(board: TestVendor {host})" not in out, "the host name is replaced")
check("Dudes Control Center system report" in out, "the program's own name is kept")
fan.close()
kbd.close()
sys.exit(check.finish())
