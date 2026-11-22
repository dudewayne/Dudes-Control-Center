# Dudes Control Center

A control center for Clevo-based notebooks on Linux: fan control through the Embedded Controller (EC), CPU power modes and frequency limits, and per-key RGB keyboard lighting. Root daemons do the hardware work; a GTK3 GUI and two command-line tools talk to them. This application was completely coded by AI with [Bionic AI Agent](https://lmstudio.ai/).

![Release](https://img.shields.io/github/v/release/dudewayne/Dudes-Control-Center)
![CI](https://github.com/dudewayne/Dudes-Control-Center/actions/workflows/ci.yml/badge.svg)
![Platform](https://img.shields.io/badge/platform-Linux-lightgrey.svg)

## Components

| Component | What it does |
|---|---|
| `dcc-fan-daemon` | Fan control via EC I/O ports: fan curves (profiles), manual speed, safety overrides. systemd service with watchdog. |
| `dcc-cpu-daemon` | CPU power modes (governor + energy preference), per-core frequency profiles, a hard frequency limit. |
| `dcc-kbd-daemon` | Keyboard backlight: whole-keyboard and per-key colors, brightness, keyboard profiles. |
| `ite_829x` kernel module (DKMS) | Driver for the ITE 829x keyboard controller: one RGB LED class device per key (`rgb:kbd_backlight*`). Written by **TUXEDO Computers** as part of [tuxedo-drivers](https://github.com/tuxedocomputers/tuxedo-drivers) (GPL-2.0-or-later), modified for this project; see [drivers/README.md](drivers/README.md). |
| `dcc-hard-limit-enforcer` | systemd timer (every 10 s) that re-applies the CPU hard limit independently of the CPU daemon. |
| `dcc-gui` | GTK3 GUI for everything above, with system tray icon. |
| `dcc-fan-cli`, `dcc-cpu-cli` | Command-line tools for the fan and CPU daemons. |

Commands and services use the short prefix **`dcc-`** (since 1.19.0); folders with your data, the package and the access group keep the full name `dudes-control-center`.

## Installation

Download the `.deb` from the [latest release](https://github.com/dudewayne/Dudes-Control-Center/releases/latest) and install it with `sudo` (so the package knows your user name):

```bash
sudo dpkg -i dudes-control-center_<version>_amd64.deb
```

The package:
- installs the daemons, GUI, CLIs and systemd units, and starts the services
- builds and loads the `ite_829x` keyboard module with DKMS (signed with your MOK key if Secure Boot is set up)
- creates the group `dudes-control-center` and adds your user to it (only its members and root may control the daemons)
- writes your user name to `/etc/dudes-control-center/user`, so the root daemons use your `~/.config/dudes-control-center/profiles.json` (see [Configuration](#configuration))

Uninstall with `sudo apt remove dudes-control-center`; `sudo apt purge dudes-control-center` also removes the configuration and logs.

Upgrading from a version before 1.19.0 stops the old `dudes-control-center-*` services and starts the new `dcc-*` ones automatically. The old command names `dudes-control-center-gui`, `-fan-cli` and `-cpu-cli` still work for now (they link to the new ones).

## GUI

```bash
dcc-gui            # starts minimized to the tray
dcc-gui --debug    # more output in GUI.log
```

| Tab | Contents |
|---|---|
| **Status** | CPU/GPU temperature, each fan's RPM and duty (from the EC), auto/manual fan mode. Manual mode: +/- in 10% steps from **0% (fans off)** to 100%. |
| **Fan Profiles** | One dropdown for all fan profiles (the one in use is marked "(Active)"): **Activate**, New, Rename, Delete, and the graphical temperature-to-fan-speed curve of the selected profile (add, edit, delete points; Apply Changes). Shows a warning with "Disable Manual Mode" while manual mode is on. |
| **CPU Profiles** | Power mode (**Performance / Balanced / Power Save**), per-core min/max frequency profiles, the CPU hard limit. |
| **Keyboard Backlight** | Keyboard profiles, **brightness slider**, backlight off switch, colors, the keyboard picture for **per-key lighting**, the LED wizard. |
| **Logs** | Live GUI, fan daemon and CPU daemon logs. |
| **About** | Version, credits, links. |

**Tray icon:** left click shows/hides the window, right click opens a menu with fan and CPU profiles. Closing the window minimizes to the tray. Window position and size are remembered; only one GUI instance can run.

### Per-key keyboard lighting

1. Run the **LED Wizard** once: it lights each LED in turn and you click the matching key on the picture. The mapping is saved in `~/.config/dudes-control-center/keyboard_led_mapping.csv`. Keys with several LEDs (Space, Enter, ...) get all of them.
2. Click keys on the picture to select them, then click a color: exactly those keys change, the others keep their color. With no keys selected, the color applies to the whole keyboard. **Clear selection** deselects all keys.
3. **Save Profile** stores the colors in the keyboard profile; **Activate Profile** brings them back. The keyboard daemon restores the active profile when it starts.
4. **Backlight Off** switches the keyboard off until you switch it on again, choose a color or activate a profile, also across reboots. Opening the GUI does not change the backlight.
5. **Brightness** (0-10) is the keyboard's hardware brightness and works on top of the colors. It is the same setting as the keyboard slider of the desktop (e.g. Linux Mint's battery/power applet), so either one can be used; the level is kept across reboots.

### CPU power modes

A power mode is a governor plus the CPU's energy performance preference (EPP), like the power profiles of GNOME/Mint/Windows:

| Mode | With `intel_pstate` (most Intel laptops) | With `schedutil` available |
|---|---|---|
| Performance | `performance` governor | `performance` governor |
| Balanced | `powersave` + EPP `balance_performance` | `schedutil` |
| Power Save | `powersave` + EPP `power` | `powersave` + EPP `power` |

Only modes the system supports are offered. Each per-core profile stores its mode (changed in the list, stored with **Save**). For the active profile a new mode takes effect right away; for another profile when it is activated. `power-profiles-daemon` also sets EPP; to give this application full control: `sudo systemctl mask --now power-profiles-daemon`.

## Command line

```bash
dcc-fan-cli                  # temperatures and fan status
sudo dcc-fan-cli --set 40    # fan speed 0,10,...,100 (0 = fans off)
sudo dcc-fan-cli --auto      # automatic, temperature-based
dcc-fan-cli --log-level DEBUG

dcc-cpu-cli                  # CPU status
dcc-cpu-cli --list-profiles
dcc-cpu-cli --apply-profile 0
dcc-cpu-cli --hard-limit 3.8 # GHz
dcc-cpu-cli --hard-limit-get
```

`--help` shows all options. `--set` and `--auto` access the EC directly and need root.

## Safety features

⚠️ These applications access hardware I/O ports to control the Embedded Controller. Use at your own risk.

1. **Watchdog**: the fan daemon pings the systemd watchdog (`WatchdogSec=30`); if its main loop hangs, systemd restarts it. Whenever the daemon stops for any reason (crash, watchdog, stop, upgrade), `ExecStopPost=` sets the fans to 100% directly through the EC.
2. **Sensor fail-safe** (auto and manual mode): invalid readings (below 10°C, above 110°C, no answer from the EC) count as failures; after 2 in a row (~10 s) the fans are held at 100% until valid readings return. Afterwards the previous mode and speed are restored.
3. **Emergency temperature** (auto and manual mode): at 95°C or more (CPU or GPU) the fans go to 100% until the temperature drops below 90°C. In manual mode the emergency **ends manual mode**: when it clears, the fan curve (auto mode) takes over instead of going back to a low manual speed. The GUI switches to auto and says why.
4. **Safe curve fallback**: a missing or empty fan profile uses the built-in default curve, never a fixed low speed.
5. **CPU hard limit**: a maximum frequency for all cores, enforced by the CPU daemon and, independently, by a systemd timer every 10 s.
6. **Access control**: the daemons accept commands only from root and members of `dudes-control-center` (checked with `SO_PEERCRED`). Files in the user's home directory are read and written with the user's identity, never through symlinks.

`STATUS` of the fan daemon reports `FAILSAFE=1` while an override is active.

## Configuration

| What | Where |
|---|---|
| Fan, CPU and keyboard profiles, CPU hard limit | `~/.config/dudes-control-center/profiles.json` |
| Keyboard LED mapping (LED wizard) | `~/.config/dudes-control-center/keyboard_led_mapping.csv` |
| GUI settings (window position etc.) | `~/.config/dudes-control-center/config.json` |
| Whose `profiles.json` the root daemons use | `/etc/dudes-control-center/user` |
| Keyboard backlight switched off (kept across reboots) | `/var/lib/dudes-control-center/kbd_backlight_off` |

The daemons run as root, so they look up the desktop user's `profiles.json` (first match wins):
1. `DCC_PROFILES_PATH` (absolute path, e.g. in a systemd drop-in)
2. the user named in `/etc/dudes-control-center/user` (written by the package)
3. the first member of the `dudes-control-center` group
4. the user with uid 1000

Each daemon logs the chosen path once: `[CONFIG] Using ...`.

### Logs

| Log | Location |
|---|---|
| GUI | `~/.cache/dudes-control-center/logs/GUI.log` |
| Fan daemon | `/var/log/dudes-control-center/FAN_DAEMON.log` |
| CPU daemon | `/var/log/dudes-control-center/CPU_DAEMON.log` |
| Keyboard daemon | `/var/log/dudes-control-center/KBD_BACKLIGHT_DAEMON.log` |
| Hard-limit enforcer (errors only) | `/var/log/dudes-control-center/HARD_LIMIT_ENFORCER.log` |

### Sockets and access

- `/run/dcc-fan.sock`, `/run/dcc-cpu.sock`, `/run/dcc-kbd.sock`
- Anyone may connect (mode 666), but only root and members of `dudes-control-center` may send commands; others get `ERROR Permission denied`. To add a user (takes effect immediately):
  ```bash
  sudo usermod -aG dudes-control-center <user>
  ```
- Text protocol, one command per line, several clients at once. Examples: fan daemon `STATUS`, `LIST_PROFILES`, `SET_MANUAL <pct>`, `SET_AUTO <0|1>`; CPU daemon `STATUS`, `LIST_GOVERNORS`, `APPLY_CUSTOM_PROFILE|<n>`, `GET_HARD_LIMIT`; keyboard daemon `STATUS`, `SET_KEYS_COLOR::<R,G,B>::<led,led,...|all>`, `GET_ZONES`, `LOAD_PROFILE::<name>`.

## Building from source

```bash
sudo apt install build-essential pkg-config libgtk-3-dev libcjson-dev python3 dkms
make -j$(nproc)          # all applications
make deb -j$(nproc)      # Debian package in build-deb/
make test -j$(nproc)     # test suite
make deb-check           # check the built package
```

`make test` builds the tests with AddressSanitizer and UBSan and runs them without root or hardware: the daemons run against a fake sysfs (CPU frequencies, 120 keyboard LEDs) in temporary directories, so it is safe on a machine with the daemons installed. GitHub Actions runs `make all`, `make test` and `make deb` on every push and pull request. Details: [tests/README.md](tests/README.md).

`sudo make install` installs the programs and systemd units without a package (no DKMS module, no `/etc/dudes-control-center/user`; the daemons then use the first member of the group). The `.deb` is the recommended way.

## Hardware

Designed for Clevo-based notebooks with EC access at I/O ports 0x62/0x66 and an ITE 829x keyboard controller. The fan code is based on the open-source [fan-control](https://github.com/steiiin/fan-control) project; the keyboard driver `ite_829x` comes from TUXEDO Computers' [tuxedo-drivers](https://github.com/tuxedocomputers/tuxedo-drivers).

**Tested on:** Intel Core Ultra 9, NVIDIA RTX 5090 Mobile, standard Clevo EC layout, Linux Mint.

### EC registers

| Register | Address | Description |
|---|---|---|
| CPU temp | 0x07 | CPU temperature in °C |
| GPU temp | 0xCD | GPU temperature in °C |
| Fan1 duty | 0xCE | CPU fan duty cycle (0-255) |
| Fan2 duty | 0xCF | GPU fan duty cycle (0-255) |
| Fan1 RPM | 0xD0/0xD1 | CPU fan RPM (high/low byte) |
| Fan2 RPM | 0xD2/0xD3 | GPU fan RPM (high/low byte) |

Fan percentage: the fan duty, from the EC's duty registers (`0xCE`/`0xCF`, 0-255) where the EC keeps them; ECs that take the speed through command `0x99` leave them at 0, and then the duty set by the fan daemon is shown. The RPM next to it is the measured speed (`2156220 / raw`). The percentage is not derived from the RPM, because the maximum RPM differs between fans and changes with dust and age.

### Default fan curve

Linear interpolation between the points: ≤45°C 20%, 55°C 30%, 60°C 40%, 70°C 50%, 80°C 70%, ≥85°C 100%. Custom curves are edited in the Fan Profiles tab.

## Troubleshooting

**GUI says a daemon is not running**
```bash
systemctl status dcc-fan-daemon dcc-cpu-daemon dcc-kbd-daemon
sudo systemctl restart dcc-fan-daemon
```

**`ERROR Permission denied` from a daemon**: your user is not in the control group. `id` should list `dudes-control-center`; if not, add it with the `usermod` command above.

**Profiles are not found / defaults are used**: check which file the daemons use (`grep CONFIG /var/log/dudes-control-center/*.log`) and correct `/etc/dudes-control-center/user` if needed, then restart the daemons.

**Keyboard backlight does nothing**: check that the module is loaded (`lsmod | grep ite_829x`, `dkms status ite_829x`) and that `/sys/class/leds/rgb:kbd_backlight*` exists. With Secure Boot the module must be signed with an enrolled MOK key. Per-key colors need the LED wizard mapping.

**Keys light the wrong LEDs**: run the LED wizard again.

**The desktop's keyboard brightness slider does nothing**: it talks to UPower, which has to see the current keyboard driver. The package restarts UPower after updating the driver; if the slider still does not react, run `sudo systemctl restart upower` (or log out and in).

**The desktop's keyboard slider only switches on and off** (after upgrading from a version before 1.20.0): the desktop's power daemon (e.g. Cinnamon's `csd-power`) remembers the old 0-255 range until the next login. Log out and in once.

**Zero or wrong temperatures, fans do not react**: your model may use a different EC layout or commands, or the BIOS may lock fan control.

**Test the connection to a daemon:**
```bash
echo STATUS | socat - UNIX-CONNECT:/run/dcc-fan.sock
```

## Credits & Acknowledgments

This application was **completely coded by AI** using [Bionic AI Agent](https://lmstudio.ai/).

### Special Thanks To:
- **[LM Studio](https://lmstudio.ai/)** - For the Bionic AI Agent that created this entire application
- **[steiiin](https://github.com/steiiin/fan-control)** - Creator of fan-control, the source of this project's fan control code
- **[agramian](https://github.com/agramian/clevo-fan-control)**, **[AqD](https://github.com/SkyLandTW/clevo-indicator)** and **Jonas Diemer** - Authors of clevo-fan-control, Clevo Fan Control Indicator and fanctrl.c, on which fan-control is based
- **[TUXEDO Computers](https://github.com/tuxedocomputers/tuxedo-drivers)** - Authors of the `ite_829x` keyboard backlight driver (part of tuxedo-drivers), which this project uses in modified form
- **Linus Torvalds** - Founder of Linux [Blog](https://torvalds-family.blogspot.com/)
- **The Great Linux Community** - For continuous support and innovation
- **[Clevo](https://www.clevo.com.tw/)** - For producing and selling awesome laptops

### Author
**Dudemeister**
- GitHub: [Dudes-Control-Center](https://github.com/dudewayne/Dudes-Control-Center)

## License

Dudes Control Center is licensed under the **GNU General Public License version 2** (GPL-2.0-only): license text in [LICENSE](LICENSE), copyright and origins in [COPYRIGHT.md](COPYRIGHT.md).

- **Fan control code**: the Embedded Controller access is based on [fan-control](https://github.com/steiiin/fan-control) by steiiin, a fork of [clevo-fan-control](https://github.com/agramian/clevo-fan-control) by agramian (GPL-2.0), which in turn is based on [Clevo Fan Control Indicator](https://github.com/SkyLandTW/clevo-indicator) by AqD and on `fanctrl.c` by Jonas Diemer. fan-control has no license file of its own; as a fork of GPL-2.0 code it is used under the GPL-2.0, which is why this project is GPL-2.0.
- **Keyboard driver**: `drivers/ite_829x.c` is copyright 2019-2023 TUXEDO Computers GmbH, from [tuxedo-drivers](https://github.com/tuxedocomputers/tuxedo-drivers), licensed GPL-2.0-or-later and used here under the GPL-2.0 ([drivers/LICENSE](drivers/LICENSE)).

The package installs `/usr/share/doc/dudes-control-center/copyright` (machine-readable, all origins and licenses) and `LICENSE`, plus the driver's license with its source (`/usr/src/ite_829x-<version>/LICENSE`).

## References

- Original fan-control: https://github.com/steiiin/fan-control
- clevo-fan-control (fan-control's origin, GPL-2.0): https://github.com/agramian/clevo-fan-control
- tuxedo-drivers (origin of the ite_829x keyboard driver): https://github.com/tuxedocomputers/tuxedo-drivers
- Tuxedo Control Center (design reference): https://github.com/tuxedocomputers/tuxedo-control-center
- GTK Documentation: https://docs.gtk.org/gtk3/
