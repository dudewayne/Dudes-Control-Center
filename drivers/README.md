# ite_829x keyboard backlight driver

`ite_829x.c` is the ITE 829x keyboard backlight driver from **[tuxedo-drivers](https://github.com/tuxedocomputers/tuxedo-drivers)** by **TUXEDO Computers GmbH** (`src/ite_829x/ite_829x.c`), copyright 2019-2023 TUXEDO Computers GmbH, licensed under the **GNU GPL version 2 or later** ([LICENSE](LICENSE)).

Dudes Control Center ships it as a DKMS module (built and loaded by the `.deb`) and has modified it; the changes are in this repository's git history. The modified driver is distributed under the same license.

The driver registers one RGB LED class device per key (`/sys/class/leds/rgb:kbd_backlight*`), which the keyboard daemon (`dcc-kbd-daemon`) uses.
