# Changelog - Dudes Control Center

All notable changes to this project will be documented in this file.

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
