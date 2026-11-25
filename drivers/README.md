# ite_829x keyboard backlight driver

`ite_829x.c` is the ITE 829x keyboard backlight driver from **[tuxedo-drivers](https://github.com/tuxedocomputers/tuxedo-drivers)** by **TUXEDO Computers GmbH** (`src/ite_829x/ite_829x.c`), copyright 2019-2023 TUXEDO Computers GmbH, licensed under the **GNU GPL version 2 or later** ([LICENSE](LICENSE)).

Dudes Control Center ships it as a DKMS module (built and loaded by the `.deb`) and has modified it; the changes are in this repository's git history. The modified driver is distributed under the same license.

The driver registers one RGB LED class device per key (`/sys/class/leds/rgb:kbd_backlight*`), which the keyboard daemon (`dcc-kbd-daemon`) uses.

# dcc_fan fan and temperature module

`dcc_fan.c` is written for Dudes Control Center (GPL-2.0-only, like the rest of the project) and is built in the same DKMS package. It loads only where Clevo's WMI interface (`ABBC0F6D-8EA1-11D1-00A0-C90629100000`) exists, unless `force=1`, and registers a hwmon device `dcc_fan`:

| File | Content |
|---|---|
| `temp1_input`, `temp2_input` | CPU and GPU temperature (EC 0x07, 0xCD) |
| `fan1_input`, `fan2_input` | CPU and GPU fan RPM (EC 0xD0/0xD1, 0xD2/0xD3) |
| `pwm1`, `pwm2` | fan duty 0-255: read = what the firmware runs (WMI GET_FANINFO1/2, byte 0), write (root) = set it |

The EC is read through the kernel's ACPI EC driver (under its lock); the speed is set through Clevo WMI `SET_FANSPEED_VALUE` (0x68), or EC command 0x99 with `method=ec`. Once a duty was written, a watchdog sets the fans to 100% when no write comes for `watchdog_sec` seconds (default 30), and so does unloading. `/sys/devices/platform/dcc_fan/control`, `wmi_faninfo` and `ec_values` show the state and raw values. The fan daemon and `dcc-fan-cli` use the module when it is loaded and fall back to port I/O otherwise.
