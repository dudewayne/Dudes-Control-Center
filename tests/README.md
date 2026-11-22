# Test suite

```bash
make test
```

Builds the tests with **AddressSanitizer + UBSan** into `build-test/` and runs
`tests/run_tests.sh`. It needs **no root and no hardware**. The daemons are built
with relative socket, sysfs and config paths, and every test runs in its own
temporary directory under `build-test/work/`. The real `/run` sockets,
`/sys` and `~/.config` are never touched. Running it on the laptop while the
installed daemons are active is safe.

Build dependencies: `build-essential pkg-config libgtk-3-dev libcjson-dev python3`.

| Suite | File | What it covers |
|---|---|---|
| `fan_responses` | `test_fan_responses.c` | Fan daemon response building: the `GET_PROFILE` overflow (v1.16.1), truncation, end markers |
| `fan_safety` | `test_fan_safety.c` | Fan control with mocked sensors/EC: sensor fail-safe, 95°C emergency override and hysteresis, built-in curve fallback, profile index handling, EC write failures, pipelined commands, non-reading clients, `sd_notify` (v1.16.2) |
| `secure_allow` / `secure_deny` | `test_secure.c` | `SO_PEERCRED` authorization (once with a group the user is in, once with a non-existent group), `SIGPIPE`, symlink-safe atomic file I/O (v1.16.1) |
| `paths` | `test_paths.c` | Which user's `profiles.json` the root daemons use: `DCC_PROFILES_PATH`, the install-time user file, group member, uid 1000; root and unknown users refused (v1.18.1) |
| `cpu_daemon` | `test_cpu_daemon.py` | The real CPU daemon against a fake 24-core, 5.3 GHz sysfs: 64-bit frequencies, hard limit as a ceiling, per-core min/max enforcement, corrupt values, several clients, long batch commands, signals (v1.16.4) |
| `kbd_daemon` | `test_kbd_daemon.py` | The real keyboard daemon against a fake 120-LED `/sys/class/leds`: per-key colors (`SET_KEYS_COLOR`), commands over 256 bytes, one zone per color, saving/restoring profiles, `TURN_ON` keeping the colors, restore at start, clean shutdown (v1.18.0); several clients, split commands, newline framing (v1.18.5); off state kept (v1.18.6); LED numbering by name and one-time conversion (v1.18.7) |
| `gui_save` | `test_gui_save.c` + `test_gui_save.py` | GUI profile saving against a fake fan daemon and the test CPU daemon: keyboard section preserved, sections kept when a daemon is unreachable, corrupt file untouched; GUI log routing (v1.16.3); power-mode matching (v1.17.1); keyboard key/LED lookup and key colors (v1.17.3, v1.18.0) |

`fakesys.py` holds the shared helpers: the fake sysfs tree and the socket client.

## When a test fails

The runner prints the failing checks plus the last lines of stdout and stderr,
including sanitizer reports. Full output is in `build-test/work/<suite>/`.
In CI the same directory is uploaded as the `test-work` artifact.

## Adding tests

- C unit tests can `#include` a daemon's `.c` file with `#define main daemon_main`
  to reach its static functions (see `test_fan_safety.c`). Mock hardware access
  with `#define` before the include.
- Paths the code uses can be overridden at build time with `-D...`:
  `SOCKET_PATH`, `CPU_SOCKET_PATH`, `CPUSYSFS`, `CONTROL_CENTER_CONFIG_DIR`,
  `CPU_LOG_DIR`, `DCC_AUTH_GROUP`.
- Print `PASS  ...` / `FAIL  ...` lines and exit non-zero on failure, then add
  a `run` line to `run_tests.sh` and a build rule to the `Makefile`.
