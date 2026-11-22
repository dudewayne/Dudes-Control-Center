# Changelog - Dudes Control Center

All notable changes to this project will be documented in this file.

## [1.17.1] - 2026-11-22

### Fixed
- **CPU governor choices now match what the system offers**: the CPU daemon always created the policies Performance, Balanced (`schedutil`) and Power Save, but the `intel_pstate` driver in active mode (common on Intel laptops) only offers `performance` and `powersave`. Choosing "Balanced" failed silently, and profiles saved with it stored `schedutil`, so their governor never applied. Policies are now only created for available governors.
- **Profiles naming an unavailable governor use the closest available one**: for dynamic governors (`schedutil`, `ondemand`, `conservative`) that is `schedutil` if present, otherwise `powersave`, which scales dynamically with load under `intel_pstate`. A warning is logged at startup and on apply. Saving a profile (GUI batch save, create, set governor) stores only available governors, so existing profiles are corrected the next time they are saved.
- The test suite now checks governor handling against a driver that offers only `performance` and `powersave` (87 checks).

## [1.17.0] - 2026-11-22

### Added
- **Automated test suite (`make test`)**: 6 suites, 82 checks, built with AddressSanitizer + UBSan. They cover the fixes from 1.16.1–1.16.4: the fan daemon response overflow, fan safety logic (mocked sensors/EC), socket authorization and symlink-safe file I/O, the real CPU daemon against a fake 24-core 5.3 GHz sysfs, and GUI profile saving and log routing against a fake fan daemon. No root or hardware needed; everything runs in temporary directories. See `tests/README.md`.
- **GitHub Actions CI** (`.github/workflows/ci.yml`): on every push and pull request it builds all applications, runs the test suite and builds the `.deb` (uploaded as a build artifact). Logs of failing tests are uploaded too.
- Socket paths in the GUI and CPU headers can be overridden at build time (used by the tests).

### Removed
- Obsolete v1.0 tests (`test.sh`, `tests/test_gui.py`, `tests/TEST_SUMMARY.md`): they used a `/tmp` socket path and an old project folder and no longer worked.

## [1.16.4] - 2026-11-22

### Fixed (CPU frequency control)
- **Frequencies above 4.29 GHz overflowed**: all frequencies were stored as 32-bit Hz. The CPU daemon, CLI and GUI now use 64-bit Hz throughout. The IPC protocol and `profiles.json` (both in Hz) are unchanged. On a 5.3 GHz CPU, `cpuinfo_max_freq` used to read as ~1.0 GHz, and hard limits above 4.29 GHz were impossible.
- **The hard limit also acted as a floor**: every 5 s the daemon raised each core's max to the hard limit, undoing lower per-core values from the active profile. The target per core is now the profile's value, capped by the hard limit (the hard limit itself if the profile sets none). Values are still re-applied every 5 s if something else changes them, but the hard limit is never exceeded. The enforcement log line only appears when something actually changed.
- **Only ~10 of 24 cores were saved**: the GUI cut CPU commands at 256 bytes, so `SAVE_CUSTOM_PROFILE` lost the remaining cores and left a truncated number in the last one (seen as "3 Hz" for core 10 in existing profiles). Commands are now unlimited in the GUI, and up to 8 KB in the daemon (enough for 64 cores).
- **Corrupt per-core values are ignored**: values below a core's hardware minimum (e.g. those truncated numbers) are treated as "not set", with a warning in the log, instead of pinning the core at its lowest frequency. **Existing profiles should be re-saved in the GUI** so that every core gets its intended value.
- **CPU daemon served only one client**: a new connection (e.g. the CLI) orphaned the GUI's connection and leaked its descriptor. The daemon now serves up to 10 clients with newline-framed commands, non-blocking sends, and `select()` instead of polling every 100 ms. Commands without a trailing newline (older scripts) still work after 200 ms. The CLI now sends a newline.
- **Profile minimum frequencies are enforced in both directions**: the daemon only ever raised a core's minimum, so stale higher minimums stayed in place (found in testing: cores 11–23 kept a 3.6 GHz minimum after the profile was re-saved with 0.8 GHz). Cores without a profile minimum are left alone.
- CPU daemon signal handlers only set flags; SIGHUP reloading runs in the main loop instead of inside the signal handler.
- `log_write` is now format-checked by the compiler; this found the 22 mismatched `%u` frequency formats fixed above.

## [1.16.3] - 2026-11-22

### Fixed
- **Saving fan/CPU settings erased the keyboard profiles**: the GUI rebuilt `profiles.json` from scratch with only the fan and CPU sections. It now merges into the existing file. `keyboard_profiles` and any other keys are preserved. A section is only replaced when the daemon returned it completely; if the fan or CPU daemon is unreachable, or a response is incomplete, the saved section is kept (before, an empty CPU section and no hard limit were written). An unparsable file is never overwritten. The hard limit is carried over if the spinner is unavailable. Writes are atomic (temp file + rename).
- **GUI log was nearly empty (Rule R2)**: all GUI output (`g_print`, `g_printerr`, GLib/GTK warnings) now goes to `~/.cache/dudes-control-center/logs/GUI.log` with proper levels, and is still echoed to a terminal if the GUI was started from one. Identical repeated messages (periodic status refresh) are logged at most once per minute. New `--debug` option for DEBUG-level detail. Startup logs the version.
- GUI never read `FAN_DUTY` from the fan daemon status (compared 11 characters of a 9-character key).
- Keyboard off-switch handler returned no value when the keyboard daemon was not connected (undefined behaviour).

## [1.16.2] - 2026-11-21

### Fixed (Fan safety)
- **Sensor failure now triggers the fail-safe**: after 2 consecutive invalid CPU temperature readings (~10 s) the fans are held at 100% in auto *and* manual mode until valid readings return. Before, the fan just stayed at its last speed. EC reads that time out are now reported as errors instead of returning a stale byte.
- **Emergency temperature override**: at 95°C or more (CPU or GPU) the fans go to 100% in any mode, including manual 0%. They stay there until the temperature drops below 90°C, then the previous setting is restored.
- **No more fixed 20% fallback**: if the active profile is missing or has no curve points, the daemon uses the built-in safe curve. Activating a profile without points is refused. Deleting a profile keeps the active profile correct; deleting the active one switches to "Default". An invalid saved active index falls back to profile 0.
- **Watchdog replaced**: the old check ran inside the loop it watched, used wall-clock time, and shut the daemon down after suspend/resume or a clock change. The daemon now pings the systemd watchdog (`WatchdogSec=30`); if it hangs, systemd restarts it. The unit's new `ExecStopPost=` sets the fans to 100% through the EC whenever the daemon stops for any reason (crash, watchdog kill, stop, upgrade). Loop stalls are measured with the monotonic clock and only logged.
- **Signal handling**: SIGTERM/SIGINT handlers only set a flag; the fans-to-100% fail-safe runs after the main loop (any mode, not just auto).
- **Clients can no longer stall fan control**: client sockets are non-blocking, and a client that stops reading responses is disconnected. All client slots are now polled (clients in higher slots were ignored after a disconnect). Pipelined commands no longer corrupt each other.
- The control step runs on a fixed 5 s monotonic timer instead of every 5th loop iteration (which ran faster while the GUI was polling).
- Curve evaluation no longer sorts the stored profile in place, so point indices used by the GUI stay stable.
- A failed EC write is logged and retried, instead of being reported as the new fan duty. `SET_AUTO 0` keeps the current speed instead of jumping to an old manual value.
- `STATUS` reports `FAILSAFE=1` while a safety override is active.

### Fixed (Packaging)
- **Keyboard module (DKMS) was never rebuilt on install**: the packaged `dkms.conf` said version 1.15.0 while the source was installed as a newer version, so `dkms add` failed silently. The deb now writes the package version into `dkms.conf`. `postinst` unregisters other module versions (including broken leftovers), installs with `--force`, then unloads the old module and loads the new one, and prints the loaded module's version and path.

## [1.16.1] - 2026-11-21

### Fixed (Security)
- **Stack buffer overflow in the fan daemon**: `GET_PROFILE` could write past its response buffer once a profile had about 32 or more curve points. Any local user could trigger it. The response is now built with strict bounds, and the response buffer was enlarged so a full 50-point profile fits. `LIST_PROFILES` and the keyboard daemon's `LIST_LEDS`/profile list use the same bounded pattern. The CPU daemon's `GET_AVAILABLE_GOVERNORS` no longer overflows or leaks, and `SET_CORE_*` rejects negative CPU ids.
- **Socket access control**: the fan, CPU and keyboard daemons now check the connecting process (`SO_PEERCRED`). Only root and members of the new `dudes-control-center` group may send commands. Others get `ERROR Permission denied` and are disconnected; a rejected connection no longer kicks out the GUI's connection. Group membership is read on every connection, so no re-login is needed. The deb `postinst` and `make install` create the group and add the installing user (`SUDO_USER`/`PKEXEC_UID`). The keyboard socket mode changed from 0777 to 0666.
- **Root no longer reads/writes `profiles.json` as root**: the daemons access the file with the identity of the config directory's owner (`setfsuid`). They never follow a symlink at the file and refuse a symlinked config directory. Writes are atomic (temp file + fsync + rename). Before, a symlink could redirect root's writes to any system file. If the existing file is not valid JSON, the daemons no longer overwrite it (that used to wipe other sections). Removed the unused CPU writer that would have replaced the whole file.
- **Hard-limit enforcer script**: now reads `profiles.json` as the directory owner (`setpriv`) and logs to `/var/log/dudes-control-center/HARD_LIMIT_ENFORCER.log` instead of writing as root into `~/.cache`. It validates the parsed limit and only logs when it changed something.
- Daemons ignore `SIGPIPE`, so a client that disconnects early can no longer kill them. When all fan-daemon client slots are full, the extra connection is closed instead of leaked.
- Parse errors on `profiles.json` no longer copy file contents into the daemon log.

## [1.16.0] - 2026-11-21

### Added
- **Visual Zone Editor**: Added a GTK/Cairo canvas in the Keyboard Backlight tab that displays the keyboard schematic (dark variant). Users can click keys to select/deselect LEDs for zone assignment. Selected keys are highlighted with a green overlay.
- **LED Wizard**: Step-by-step wizard to assign each of the 120 LEDs to a key position on the keyboard schematic. The wizard lights up the current LED in cyan using the hardware, highlights the clicked key on the canvas, and persists the mapping to a CSV file (`~/.config/dudes-control-center/keyboard_led_mapping.csv`) for easy manual editing. Supports Skip, Save & Finish, and Cancel buttons. Mapped keys are shown with a blue highlight on the canvas after the wizard completes.

## [1.15.0] - 2026-11-21

### Added
- **Keyboard Backlight Off Switch**: Added a toggle switch in the Keyboard Backlight tab to disable/enable the keyboard backlight entirely. When enabled, the off switch overrides all profile settings and stays active until the user disables it.

## [1.14.0] - 2026-11-21

### Added
- **Keyboard Backlight Control**: Added a new keyboard backlight daemon (`dudes-control-center-kbd-backlight-daemon`) and a new tab in the GUI for managing keyboard backlight profiles. Supports zone-based color profiles with a keyboard schematic visualization. Profiles are stored in `profiles.json` alongside fan and CPU profiles.

## [1.13.1] - 2026-11-21

### Fixed
- **Governor failure no longer blocks profile activation**: When activating a CPU profile via the GUI, if the governor set in the profile is not available on the hardware (e.g., schedutil on systems with only performance/powersave), the daemon now logs a warning and continues to apply the per-core frequencies instead of failing the entire profile activation.

## [1.13.0] - 2026-11-21

### Added
- **CPU CLI tool (dudes-control-center-cpu-cli)**: Added a command-line interface for controlling CPU frequency profiles. Supports showing CPU status, listing custom profiles, applying profiles, switching governors (with abbreviation support: perf, sched, pwr), and setting/getting the hard frequency limit.

## [1.12.0] - 2026-11-20

### Added
- **CPU Profiles in system tray menu**: Added a "CPU Profiles" submenu to the system tray context menu, similar to the existing "Fan Profiles" submenu. Allows switching CPU frequency profiles directly from the tray icon.

## [1.11.0] - 2026-11-20

### Added
- **Hard limit fail-safe enforcer**: Added an independent systemd timer + script that enforces the CPU hard frequency limit every 10 seconds via sysfs. This acts as a fail-safe if the daemon or GUI crashes, ensuring the hard limit is always enforced at the system level.

## [1.10.0] - 2026-11-20

### Added
- **Rename Profile button in Curve Editor**: Added a "Rename Profile" button next to the "Delete Profile" button. Opens a dialog pre-filled with the current profile name. Supports names with spaces. Cannot rename the Default profile.

## [1.9.2] - 2026-11-20

### Fixed
- **Real-time sync of core 0 frequency values to all cores**: When "Apply values to all CPU cores" is enabled, typing a value in core 0's min or max frequency field now automatically updates all other core fields in real-time. Switched from `changed` signal to `input` signal on GtkSpinButton to ensure GTK validates the text before syncing.

## [1.8.9] - 2026-11-20

### Fixed
- **Restructured profiles.json for symmetry**: Moved `active_profile_index` for fan profiles inside the `fan_profiles` section (nested object), matching the `cpu_freq_profiles` structure. Both sections now use `active_profile_index` and `profiles` as inner keys. Backward compatibility ensures old format files are loaded correctly and migrated on first save.

## [1.8.4] - 2026-11-20

### Fixed
- **Governor always saved as schedutil**: The `load_cpu_governors()` function was truncating each line at `(` before extracting the `gov=xxx` string, so the governor parser never found the value and always fell back to the default `schedutil`. Moved the governor extraction before the line truncation.

## [1.8.3] - 2026-11-20

### Fixed
- **Governor not saved when saving CPU custom profile**: The governor field always saved as `schedutil` regardless of selection. Added `SET_PROFILE_GOVERNOR` daemon command and updated the GUI save handler to read the governor from the combo box and send it before persisting the profile.

## [1.8.0] - 2026-11-19

### Added
- **Configurable hard CPU frequency limit**: The hard max frequency safety limit (previously hardcoded at 3.6 GHz) is now configurable via a new setting in the CPU Profiles tab. Includes a warning label about hardware risk. The limit is persisted to `~/.config/dudes-control-center/cpu/hard_limit.conf` and loaded from the daemon on GUI startup.

## [1.7.1] - 2026-11-19

### Fixed
- **Enter key in profile dialogs**: Added Enter key support for creating and renaming profiles in the CPU tab and curve editor. Pressing Enter now triggers the action button instead of requiring a mouse click.
- **Selection changed after renaming CPU profile**: After renaming a profile, the combo box now stays on the renamed profile instead of switching to whichever profile the daemon marked as active.
- **Removed CPU Auto feature**: Removed the Auto toggle switch from the CPU tab since it serves no purpose when using custom frequency profiles.

## [1.7.0] - 2026-11-19

### Added
- **Spaces supported in profile names**: Changed the IPC protocol between the GUI and CPU daemon from space-delimited to pipe-delimited (`|`), allowing profile names to contain spaces without truncation.

## [1.6.13] - 2026-11-19

### Fixed
- **Apply values to all cores not default on new profile creation**: When creating a new CPU frequency profile, the "Apply values to all cores" option is now enabled by default, matching the behavior of loading existing profiles where this option is the default.

## [1.6.10] - 2026-11-19

### Fixed
- **New curve profile not auto-selected after creation**: When creating a new profile in the curve editor, the GUI stayed on the previous profile instead of switching to the newly created one with a cleared curve points list. Root cause: profile name parsing in `refresh_profile_list()` included a leading space (e.g., `' 213'` instead of `'213'`), so the `strcmp` lookup failed. Fixed by skipping the leading space after the bracket marker.

## [1.6.9] - 2026-11-19

### Fixed
- **Logs written to stdout/stderr instead of log files**: All log output (especially from the fan daemon auto-mode loop running every 5 seconds) was going to stdout/stderr (systemd journal) instead of the designated log files. Fixed by removing stderr dual-write from `log_write()` and replacing all direct `fprintf(stderr)/printf` calls in the fan daemon and GUI with `LOG_*` macros. Logs now correctly go to `/var/log/dudes-control-center/` for daemons and `~/.cache/dudes-control-center/logs/` for the GUI.

## [1.6.8] - 2026-11-18

### Fixed
- **CPU profile max frequency truncated to 2147 MHz**: Any max frequency above ~2147 MHz (2,147,483,647 Hz) was stored correctly but read back as a truncated value due to signed 32-bit integer overflow when parsing JSON. Root cause: cJSON `valueint` is `int32_t` (max 2,147,483,647), so values like 2,400,000,000 Hz overflowed. Fixed by using `valuedouble` instead, which can represent these values exactly.

## [1.6.7] - 2026-11-18

### Fixed
- **CPU core frequency grid not populated on tab switch or after delete**: The grid remained empty on first visit to the CPU Profiles tab and after deleting a profile. Root cause: GTK3 lazy-realizes notebook pages, so building the grid during the `switch-page` callback failed silently. Grid population is now deferred via `g_idle_add` until widgets are fully realized.

## [1.6.6] - 2026-11-18

### Fixed
- **CPU core frequency grid not populated on tab switch or after delete**: When switching to the CPU Profiles tab, the per-core frequency grid was empty until the user manually switched profiles. After deleting a profile, the grid was only cleared but not repopulated with the new active profile. The page-switch callback and delete handler now rebuild the grid with the active profile's data.
- **CPU core grid empty on GUI startup**: On fresh GUI launch, the per-core frequency grid remained empty because the first CPU refresh timer (5s interval) hadn't fired yet. Added an immediate STATUS query on CPU daemon connection so core data is available before the user switches tabs.

## [1.6.5] - 2026-11-18

### Fixed
- **CPU profile name comparison failed with stored display names**: The combo box stored display names with markers (`[*] ProfileName` or `     ProfileName`), but duplicate checks and profile lookups compared against raw names. Added `cpu_profile_name_from_display()` helper to strip markers and whitespace before comparison. Also fixed the rename dialog which showed incorrect default text.

## [1.6.4] - 2026-11-18

### Fixed
- **CPU profile editor not loaded after creation**: When creating a new CPU frequency profile, the per-core frequency grid was not populated with the profile's data. The editor now automatically loads the newly created profile's min/max values.

## [1.6.3] - 2026-11-18

### Fixed
- **CPU profile names included metadata**: The profile names in the combo box included daemon metadata like `(gov=schedutil, cores=24, all_cores=0)`. The `load_cpu_custom_profiles()` function now strips the parenthesized metadata before displaying, so only the marker and profile name are shown.

## [1.6.2] - 2026-11-18

### Fixed
- **CPU custom profiles not selected on tab switch**: When switching to the CPU Profiles tab, the active CPU profile was not loaded or selected in the profile list. The `load_cpu_custom_profiles()` function now detects the `[*]` active marker and selects the correct profile.

## [1.6.1] - 2026-11-18

### Fixed
- **CPU custom profiles not loading after fan_profiles rename**: The `cpu_custom_profile_load_json()` function only handled the nested JSON format, but the GUI writes `cpu_freq_profiles` as a flat array. The loader now handles both flat and nested formats to ensure compatibility.

## [1.6.0] - 2026-11-17

### Added
- **Per-core frequency profile management**: Full UI for creating, deleting, and applying per-core min/max frequency profiles on the CPU Profiles tab.
- **Per-core frequency grid**: 3-column grid (Core | Min MHz | Max MHz) with spinners (800–3600 MHz, step 100) for each core.
- **Apply to All Cores checkbox**: Copies first core's min/max values to all cores for quick uniform configuration.
- **Profile creation dialog**: Modal dialog with duplicate-name detection and confirmation message box.
- **Profile deletion with confirmation**: Yes/No dialog before deleting a frequency profile.

## [1.5.0] - 2026-11-17

### Added
- **Per-core frequency live view on Status tab**: Added a 4-column grid display showing real-time frequency values for all 24 CPU cores, updating every 5 seconds alongside other status data.

### Fixed
- **Per-core frequency display empty on Status tab**: Labels were created but not shown. Added `gtk_widget_show()` per label and `gtk_widget_show_all()` on grid.
- **Current Frequency label on CPU tab showed "N/A"**: Reconnected to first core's frequency after per-core parser replaced old single-core parse.

## [1.4.5] - 2026-11-17

### Fixed
- **Current Frequency label on CPU tab showed "N/A"**: The per-core parser replaced the old single-core frequency parse but never populated the `freq_values` label. Now connected to the first core's frequency.

## [1.4.4] - 2026-11-17

### Fixed
- **Per-core frequency display empty on Status tab**: The per-core labels were created but not shown. Added `gtk_widget_show()` for each label and `gtk_widget_show_all()` on the grid after creation. Increased section height and made it expandable.

## [1.4.3] - 2026-11-17

### Fixed
- **CPU auto-switch toggle crash**: The `state-set` signal callback on `GtkSwitch` was missing the `gboolean` parameter, causing a segfault at offset `0x159`. Fixed the callback signature and added proper signal blocking/unblocking to prevent infinite recursion.
- **CPU auto-switch toggle non-functional**: Added connection check before sending commands, proper error handling with switch revert on failure, and sync of switch state with daemon via `parse_cpu_status`.

## [1.4.2] - 2026-11-16

### Added
- **CPU auto-switch toggle**: Added a toggle switch on the CPU tab to enable/disable automatic governor switching based on temperature/load conditions.

### Fixed
- **CPU auto-switch toggle signal handler**: Initial implementation with correct `GtkSwitch` signal callback signature.

## [1.4.1] - 2026-11-16

### Fixed
- **CPU status refresh not updating after first load**: The CPU daemon closed the client connection after every response, so subsequent timer refreshes read from a dead socket. Fixed by wrapping the read/process/write loop in `handle_client()` to keep the connection alive, matching the fan daemon's behavior.

## [1.4.0] - 2026-11-16

### Added
- **CPU Speed/Frequency Control daemon (`dudes-control-center-cpu-daemon`)**: A separate daemon for CPU speed control has been integrated. This is the first step toward a full Control Center architecture with multiple subsystems, each running its own daemon for fault isolation.
  - New daemon binary: `dudes-control-center-cpu-daemon`
  - New systemd service: `dudes-control-center-cpu-daemon.service`
  - Unix socket: `/run/dudes-control-center-cpu.sock`
  - Config directory: `~/.config/dudes-control-center/cpu/`
  - CPU common module (`dudes-control-center-cpu-common.c/.h`) for all CPU control functions
  - Hard safety limit: 3.6 GHz (`HARD_MAX_FREQ_HZ`)
  - Auto-check interval: 5 seconds
  - Supports all governor and frequency commands from the reference project

### Changed
- **Version numbering corrected**: CPU daemon integration is a feature, so the minor version was bumped from 1.3.x to 1.4.0.

## [1.3.1] - 2026-11-16

### Changed
- **Renamed fan components for Control Center architecture**: The existing daemon, CLI, and all references were renamed to include the "fan" sub-component identifier. This prepares the application for adding CPU Speed Control and other subsystems under the same Control Center umbrella.
  - Daemon binary: `dudes-control-center-daemon` → `dudes-control-center-fan-daemon`
  - CLI binary: `dudes-control-center-cli` → `dudes-control-center-fan-cli`
  - Unix socket: `/run/dudes-control-center.sock` → `/run/dudes-control-center-fan.sock`
  - Systemd service: `dudes-control-center-daemon.service` → `dudes-control-center-fan-daemon.service`
  - PID file: `/run/dudes-control-center-daemon.pid` → `/run/dudes-control-center-fan-daemon.pid`
  - All internal constants, error messages, and scripts updated to match
- **Cleaned up legacy build artifacts**: Removed stale `DEBSRC/` and `build-deb/` folders containing old naming conventions.

## [1.3.0] - 2026-11-16

### Added
- **Suspend/wake recovery**: The GUI now properly recovers after the system wakes from suspend. On socket errors, the connection is marked as broken and the GUI auto-reconnects every 2 seconds in the refresh timer callback.

## [1.2.9] - 2026-11-16

### Changed
- **Reversed log display order**: Log entries in the Log tab now display with the newest content at the top instead of the bottom. New log entries are prepended on refresh, and the view auto-scrolls to the top so you always see the latest entries immediately.

### Fixed
- **Log refresh performance bug**: Replaced per-line buffer inserts with a single bulk insert for both GUI and daemon log refreshes. This prevents GTK main loop blocking during frequent log updates, ensuring the status tab and curve editor remain responsive while the log tab is active.
- **Log display read from wrong path**: `load_gui_logs` used `g_get_user_data_dir()` which resolves to `~/.local/share/`, but the log module writes to `~/.cache/`. The log display was reading a stale file from a previous session. Fixed by using the same `~/.cache/dudes-control-center/logs/` path as the log module.
- **Log lines merging together**: Occasionally, two log entries would appear on the same line in the log display. Root cause: the reversed string prepended to the buffer had no trailing newline, so the last line of the new chunk merged with the first line of the existing buffer. Fixed by appending a trailing newline to both GUI and daemon log buffers.

## [1.2.8] - 2026-11-15

### Changed
- **Window position storage migrated to JSON**: Window position and size are now saved to `~/.config/dudes-control-center/config.json` instead of `window.cfg`. This provides a consistent JSON-based configuration format across the application.

## [1.2.7] - 2026-11-15

### Fixed
- **Duplicate daemon instances**: Added PID file protection (`/run/dudes-control-center-daemon.pid`) to prevent multiple daemon instances from running simultaneously. The PID file is cleaned up on both normal shutdown and signal-triggered exit. This was the likely root cause of the manual mode switch flipping back to OFF, as the second daemon could have a different state than the first.

### Added
- **Debug logging for manual mode**: Added detailed logging in `on_manual_mode_switched()`, `refresh_timer_callback()`, and `parse_status()` to trace the state of the manual mode switch during STATUS polls.

## [1.2.6] - 2026-11-15

### Added
- **Profile names with spaces**: Profile names like "Damn Cool" are now fully supported. Previously, the daemon's command parser used space-delimited `sscanf` which truncated names at the first space. Fixed by extracting the profile name from the original command string in the daemon, and using manual string parsing in the GUI for `LIST_PROFILES` responses.

## [1.2.5] - 2026-11-15

### Fixed
- **Crash when creating a new profile with unsaved curve changes**: If curve points were modified and a new profile was created, the unsaved changes dialog would fire during `refresh_profile_list()` while the create-profile dialog was still open, causing a crash from nested modal dialogs. Fixed by handling unsaved changes at the start of `on_create_profile_clicked()` before any dialogs or daemon communication.
- **Unsaved changes dialog not shown when clicking X to close window**: Clicking the window close button (X) would hide the window to the system tray without checking for unsaved curve point changes. The unsaved changes check was only triggered when switching tabs or profiles, not when closing the window. Fixed by adding `check_unsaved_changes()` call in `on_window_delete_event()` before hiding the window.

## [1.2.4] - 2026-11-15

### Fixed
- **Curve editor not clearing after profile creation**: After creating a new profile, the combo box correctly switched to the new profile but the curve editor still showed the old profile's data. Root cause: in `refresh_profile_list()`, the `refreshing_profile_list` guard flag was cleared after the combo box signal was unblocked, causing the `changed` callback to fire while the flag was still set and return early without loading curve points. Fixed by clearing the flag before unblocking the signal, and added an explicit `load_curve_points_for_profile()` call after profile creation as a safety net.

## [1.2.3] - 2026-11-15

### Fixed
- **Unsaved changes dialog triggered after profile creation**: Creating a new profile could trigger the unsaved curve changes dialog when closing the window, even though no curve edits were made. Root cause: the combo box signal was unblocked too early in `refresh_profile_list()`, allowing the `changed` callback to fire after early returns had already cleared the `refreshing_profile_list` guard. Fixed by keeping the signal blocked until the end of the refresh function.

## [1.2.2] - 2026-11-15

### Fixed
- **Tray menu not updated after profile create/delete**: New or deleted profiles only appeared in the tray context menu after a full application restart. Added `update_tray_menu()` calls after profile creation and deletion.
- **New profile not activated automatically**: After creating a profile, the combo box stayed on the old profile. The newly created profile is now automatically activated in the daemon.

## [1.2.1] - 2026-11-15

### Fixed
- **Profiles tab empty on first visit**:
 Fixed a bug where the Profiles tab showed "No profiles available" when navigating to it immediately after a fresh GUI start. Profiles only appeared after switching away and back.
  - Root cause: GTK3 lazy-realizes notebook pages. The `switch-page` signal fires before the new page's widgets are fully realized, causing `refresh_profile_list()` to bail out at the `gtk_widget_get_realized()` check.
  - Fix: Defer profile list refresh using `g_idle_add()` so it runs after GTK has finished realizing the page widgets.
  - Applied to all three entry points: tab switch, tray left-click, and tray "Show Application" menu item.

## [1.2.0] - 2026-11-15

### Changed
- **JSON-based profile storage**: Replaced custom text-based profile format (`.dat`) with JSON (`.json`)
  - Profiles are now stored as human-readable JSON at `~/.config/dudes-control-center/profiles.json`
  - JSON includes version tracking (`"version": "1.0"`) for future migration support
  - Added `libcjson` as a runtime dependency for JSON parsing

## [1.1.1] - 2026-11-15

### Added
- **Runtime log level control for daemon**
  - Daemon socket commands: `SET_LOG_LEVEL <ERROR|WARN|INFO|DEBUG>` and `GET_LOG_LEVEL`
  - CLI subcommands: `--log-level <LEVEL>` and `--log-level-get`
  - Log level changes are audited in the daemon log file
  - Log level can be changed at runtime without restarting the daemon
  - `daemon_send_command()` helper added to common module for reusable socket communication

### Changed
- Made `log_level_to_string()` public in the logging module (was static)


## [1.1.0] - 2026-11-15

### Changed
- **Full project rename**: "Dudes Fan Control" → "Dudes Control Center"
  - All source files renamed from `dudes-fan-control-*` to `dudes-control-center-*`
  - All header guards updated (`DUDES_FAN_*` → `DUDES_CONTROL_CENTER_*`)
  - Systemd service renamed to `dudes-control-center-daemon.service`
  - Desktop entry renamed to `dudes-control-center.desktop`
  - Config directory changed from `~/.config/dudes-fan-control/` to `~/.config/dudes-control-center/`
  - Log directory changed from `/var/log/dudes-fan-control/` to `/var/log/dudes-control-center/`
  - Socket path changed from `/run/dudes-fan-control.sock` to `/run/dudes-control-center.sock`
  - Icon file renamed from `DudesFanControl.png` to `DudesControlCenter.png`
  - Package name changed to `dudes-control-center`
  - All documentation, build system, and scripts updated
  - DEBSRC legacy build structure updated with new naming

## [1.0.0] - 2026-11-14

### Initial Release
- Fan Control daemon with EC (Embedded Controller) communication
- GTK3 GUI application with:
  - Real-time temperature monitoring (CPU & GPU)
  - Fan speed monitoring (RPM & percentage)
  - Manual fan control mode
  - Multiple profile support with custom temperature curves
  - Curve editor with visual graph
  - Profile management (create, delete, switch)
  - Built-in log viewer (GUI & Daemon logs)
  - System tray integration
- CLI tool for basic fan control
- Logging system with rotation (GUI & Daemon)
- Systemd service for daemon auto-start
- Unix socket IPC between GUI and daemon
- Watchdog timer and fail-safe mode in daemon
