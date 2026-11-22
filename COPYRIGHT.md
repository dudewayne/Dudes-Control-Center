# Copyright and license

**Dudes Control Center**
Copyright (C) 2026 Dudemeister and contributors

Dudes Control Center is free software; you can redistribute it and/or modify it under the terms of the **GNU General Public License version 2** as published by the Free Software Foundation (SPDX: `GPL-2.0-only`). The full license text is in [LICENSE](LICENSE).

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

## Third-party code

- **Fan control / Embedded Controller access** (`src/dcc-common.c`, used by the fan daemon and CLI) is based on [fan-control](https://github.com/steiiin/fan-control) by steiiin, a fork of [clevo-fan-control](https://github.com/agramian/clevo-fan-control) by agramian (GPL-2.0), which is based on [Clevo Fan Control Indicator](https://github.com/SkyLandTW/clevo-indicator) by AqD and on `fanctrl.c` by Jonas Diemer. fan-control has no license file of its own; as a fork of GPL-2.0 code it is used under the GPL-2.0, which is why this project is GPL-2.0.
- **Keyboard driver** `drivers/ite_829x.c`: Copyright (c) 2019-2023 TUXEDO Computers GmbH, from [tuxedo-drivers](https://github.com/tuxedocomputers/tuxedo-drivers), licensed GPL-2.0-or-later and used here under the GPL-2.0. See [drivers/LICENSE](drivers/LICENSE).
- **Fan curves**: the fan curves, the default curve (`src/dcc-fan-daemon.c`, `src/dcc-fan-cli.c`) and the curve editor are this project's own work (Dudemeister), not taken from clevo-fan-control's Tuxedo Control Center based fan profiles.

The Debian package installs a machine-readable summary as `/usr/share/doc/dudes-control-center/copyright`.
