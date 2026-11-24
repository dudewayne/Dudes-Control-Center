# Changelog - Dudes Control Center

All notable changes to this project are documented in this file. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses [Semantic Versioning](https://semver.org/): a **Feature** raises the minor version (1.31.0 → 1.32.0), a **Fix** the patch version (1.32.0 → 1.32.1). Feature versions, and fixes that matter for every installation, are published as [GitHub releases](https://github.com/dudewayne/Dudes-Control-Center/releases) with the tested `.deb`. Versions with a git tag (all from 1.15.0 on, some earlier ones) link to the changes since the previous tag.

Programs and services were named `dudes-control-center-*` before 1.19.0 (for example `dudes-control-center-fan-daemon`, now `dcc-fan-daemon`); older entries use the old names.

## [Unreleased]

## [1.34.2] - 2026-11-24

### Changed
- **CPU tab: note on low maximum frequencies.** A maximum is a ceiling, not a target, and many CPUs do not go below a certain speed once they have work to do, however low the maximum is. Measured on the Core Ultra 9 275HX: about 2.2 GHz on the P-cores and 1.5 GHz on the E-cores (also at 1% load), while maximums above that were kept exactly; the kernel had the lower maximum set on every core. The note is shown below *Apply values to all cores* in the per-core frequency profiles and as the tooltip of the hard limit; translated into all UI languages.

### Documentation
- README troubleshooting: *A low maximum frequency seems to be ignored*, including the X58xWNx firmware's sustained 46 W CPU power limit, which lower limits set through `/sys/class/powercap` (MSR and MMIO) do not change. The man pages `dcc-cpu-cli(1)` and `dcc-cpu-daemon(8)` explain the floor too.

## [1.34.1] - 2026-11-24

### Fixed
- **Keyboard white after standby**: on resume the `ite_829x` driver restored every key's color and then sent its built-in color mode again (the one the keyboard's color key cycles through, white by default), which painted all keys white and overwrote the stored colors, so the daemon and the GUI saw white too. The driver now only restores the stored per-key colors and brightness; it re-sends its mode only for the firmware's random color effect, which is not stored per key. Colors chosen with the keyboard's color key are stored per key as well and come back too. The package update rebuilds and reloads the module and restarts the keyboard daemon, which lights the active profile again.

## [1.34.0] - 2026-11-22

### Added
- **Help tab** between Logs and About, with the sub-tabs **README**, **Changelog** and **License**, switched like the log views. README and Changelog are Markdown and shown formatted: headings, bold, italic, `code`, lists, quotes, code blocks and tables (small tables in aligned columns, wide ones one row per line); badges and images are left out, a linked screenshot becomes a link with its caption. Links can be clicked: web links open in the browser, the README's contents line and `#` links jump to the heading, `LICENSE` and `CHANGELOG.md` switch the sub-tab, other files of the repository open on GitHub, and the Changelog's version headings open the changes on GitHub. A **search field** finds text in the shown sub-tab (Enter: next match, Shift+Enter: previous; case is ignored, the search wraps around).
- The package installs `README.md` and `CHANGELOG.md` next to `LICENSE` in `/usr/share/doc/dudes-control-center/`, where the Help tab reads them (once, when the tab is first opened). A missing file is named in the sub-tab.
- Translations of the new texts (tab names, search, messages); the documents themselves stay in English.
- Tests: the Markdown display (headings, inline formatting, lists, code blocks, tables, reference links, images, anchors), where each kind of link leads, the real README, Changelog and License load without leftover Markdown syntax, and the search; `make deb-check` checks the three files and that the packaged Changelog has an entry for the package version.

## [1.33.0] - 2026-11-22

### Added
- **Man pages** for all programs, installed with the package under `/usr/local/share/man`: `dudes-control-center(7)` (overview: components, access group, profiles, files), `dcc-gui(1)`, `dcc-fan-cli(1)`, `dcc-cpu-cli(1)`, `dcc-kbd-cli(1)`, `dcc-fan-daemon(8)` (with the safety mechanisms), `dcc-cpu-daemon(8)` (with the hard limit enforcer), `dcc-kbd-daemon(8)` and `dcc-component(8)`. They are written in roff (`man/`), get the version filled in at build time and are installed compressed; `make install` and `make uninstall` handle them too.
- `make man-check` (also in CI) checks that every page renders without groff warnings; `make deb-check` checks that all nine are in the package with the version filled in.

### Documentation
- **README**: a no-warranty disclaimer at the top; the AI credits say the application was initially coded with Bionic AI Agent and that code review, hardening, bug fixing and the final polish were done by Claude (Anthropic); screenshots of all tabs (`docs/screenshots/`); a requirements box, a contents line, a quick start, a Settings section and a screenshot at the top; `gettext` added to the build dependencies; a note on the tray icon under GNOME; shorter safety and license sections that point to COPYRIGHT.md.
- **COPYRIGHT.md**: the line about the fan curves is gone. The project's own work needs no entry in the third-party list.

## [1.32.1] - 2026-11-22

### Fixed
- **Log views: "g_utf8_validate ... failed"**: a log view read its file from an arbitrary byte (the first load starts 10 KB before the end) up to the current end, which may be a line still being written. A cut inside a multi-byte character (°, ü, ß, ...) made GTK reject the text and log an error. The views now take only complete lines (the rest is read on the next refresh) and replace invalid bytes.
- **Fan curve graph: all curve points orange**: only the first point was drawn orange; the others were filled with the outline color of the previous point and looked like gaps in the line.
- Tests for the log line handling.

## [1.32.0] - 2026-11-22

### Added
- **`dcc-kbd-cli`**: command-line tool for the keyboard backlight, like `dcc-fan-cli` and `dcc-cpu-cli`. Without options it shows the status (on/off, brightness, active profile, number of LEDs); `--on`, `--off`, `--brightness <level>`, `--color <color>` (whole keyboard in one color, not saved: `R,G,B`, `#RRGGBB` or white/red/green/blue/yellow/cyan/magenta/orange/purple), `--list-profiles` (active one marked), `--profile <name>` (activate) and `--show-profile <name>` (base color and zones). It talks to the keyboard daemon's socket, so members of the `dudes-control-center` group can use it without root; clear messages when the daemon is not running or the user lacks the group.
- Tests: the CLI against the test keyboard daemon (status, brightness, colors, profiles, on/off, invalid input); `make deb-check` checks that it is installed.

## [1.31.0] - 2026-11-22

### Added
- **UI languages: German, English (UK), Spanish, Japanese and Korean**, besides the original English (US). The Settings tab has a **Language** section above Appearance: *System default* (the desktop's language) or one of the languages, stored in `config.json`. The texts are set when the window is built, so a change takes effect after a restart; **Restart now** restarts the GUI right away (after the usual questions about unsaved changes).
- All visible texts (tabs, sections, buttons, labels, tooltips, dialogs, status messages; about 316) are translatable through gettext: `po/dudes-control-center.pot`, `po/<lang>.po`, installed as `/usr/share/locale/<lang>/LC_MESSAGES/dudes-control-center.mo`. Logs, daemon commands and the key names on the keyboard pictures stay as they are.
- The **Japanese and Korean translations were made without a native speaker's review**; the Settings tab says so. Corrections go into `po/tools/translations.py` (`make po-update` regenerates the files).
- Tests: the catalogs load (German, Spanish, Japanese, Korean, English UK spelling), "English (US)" shows the original texts, translated format strings keep their placeholders; `make deb-check` checks that all five catalogs are installed. The package build checks every catalog with `msgfmt -c`.

### Fixed
- The GUI restarted with "Restart now" is not mistaken for a second instance (same process ID).
- **Fan curve graph texts with Pango**: the axis labels and titles were drawn with cairo's simple text functions, which have no font fallback, so Japanese and Korean showed as boxes. The vertical "Fan Speed (%)" title ran into the "60%" label; it is now centered at the left edge, and the percentage labels are right-aligned next to the axis.
- The curve point list is as wide as its columns (they size themselves to their headers; fixed widths and a fixed-width scroll area cut off the translated "Fan Speed (%)"). The fan curve graph fills the rest of the row instead of always taking 1000 px.

## [1.30.0] - 2026-11-22

### Added
- **Status tab: Active Profiles** next to the Per-Core CPU Frequencies (both sections the same width): the active fan profile (with the speed while manual mode is on), the active CPU profile, the power mode (governor and EPP, as on the CPU tab) and the active keyboard profile. Updated every 2 seconds; a switched-off or unreachable component is shown as such in grey.

## [1.29.1] - 2026-11-22

### Fixed
- **About page with the logo**: the Dudes Control Center logo (from the high-resolution picture, with a transparent background so it also looks right on dark themes) sits in the middle under the application name, with the version below the name. The credits are left and right of it, each column centered on the logo: Claude and the code credits on the left, Bionic AI Agent, Linux and Clevo on the right. Below the logo come "This application was completely coded by AI", the contact and the license.
- New credit: coded with **Claude** (Claude Code) by **Anthropic**.
- The logo is installed as `/usr/share/dudes-control-center/logo.png`.

## [1.29.0] - 2026-11-22

### Added
- **Appearance setting** in the Settings tab: **Follow the system** (default), **Dark** or **Light**, stored in `config.json`. Dark and light switch the GTK theme of the app only: to the installed dark or light variant of the desktop theme (e.g. Mint-Y-Aqua and Mint-Y-Dark-Aqua), otherwise through GTK's dark-theme preference. Following the system also follows later desktop theme changes: the app watches the desktop's theme setting (`org.cinnamon.desktop.interface gtk-theme`, otherwise GNOME's) and applies a new theme right away; with Dark or Light it switches to the new theme's matching variant. The note under the setting shows the theme in use.
- **Light keyboard pictures** for all layouts: the German one as provided, UK, Spanish, Japanese and Korean made from their dark pictures with the same colors. The keyboard picture switches with the theme.
- The **fan curve graph** is drawn in dark colors on dark themes (it was always white).

### Fixed
- About page: the title and "Contact" no longer use a fixed dark blue that was hard to read on dark themes.
- Tests for the theme variants and the light pictures.

## [1.28.0] - 2026-11-22

### Added
- **Keyboard layouts: German (DE), English (UK), Spanish (ES), Japanese (JP), Korean (KR)**: the Settings tab has a layout dropdown; the keyboard picture, the key names and the clickable key areas follow it. On the first start the layout is preselected from the system's keyboard layout (`/etc/default/keyboard`: de/at/ch, gb/uk/ie, es, jp, kr), otherwise German; the choice is stored in `config.json`.
- Layouts are data now instead of a compiled-in German table: `/usr/share/dudes-control-center/layouts/<layout>/keys.csv` (key positions) and `keyboard_dark.svg` (picture, drawn with GTK's SVG loader; the package depends on `librsvg2-common`).
- **LED mapping per layout** (`keyboard_led_mapping_<layout>.csv`): German keeps the checked built-in mapping; the other layouts get one derived from it, not checked on real keyboards. Keys are matched first by name (function, navigation and modifier keys; Strg = Ctrl) when they sit at nearly the same place, the rest row by row in order, each key at most once. Every UK and Spanish key gets an LED; keys only Japanese or Korean keyboards have stay without one until the LED Wizard ran on such a keyboard: Japanese `\` (Ro), Muhenkan, Henkan, Katakana/Hiragana; Korean `\` above Enter. The Settings tab says what the mapping of the selected layout is based on. Zones and profiles are unaffected by the layout (they store LED numbers).
- Tests for all five layouts, the CSV reader, the derived mappings and the conversion.

### Changed
- The existing German LED mapping (`keyboard_led_mapping.csv`) is converted once to the German layout file; the old file stays for older versions. Keys with several LEDs are one key with several LEDs now (the old table had one entry per LED).
- `config.json` keeps all its settings when the window position is saved (it used to be rewritten with only the window position).

## [1.27.0] - 2026-11-22

### Added
- **Settings tab** (between Keyboard Backlight and Logs): switch **CPU control, fan control or the keyboard backlight** off (listed alphabetically like the tabs and the tray menu) when it does not work on the laptop or causes problems, and on again. Each switch shows whether its daemon is running. Switching asks for the administrator password (polkit, remembered for a few minutes) and takes effect right away.
- A switched-off component's daemon is stopped (CPU: also the hard-limit timer) and does not start at boot or after a package update; the GUI hides its tabs (fan: Status and Fan Profiles) and its tray submenu, does not try to reconnect, and shows "switched off" instead of a red "not connected".
- **Fan control switched off**: the fans run at 100% until the next reboot (the fail-safe), then the laptop's firmware controls them. No EC command is known that hands the fans back to the firmware earlier (checked against clevo-fan-control, fan-control and clevo-indicator); the GUI asks before switching fan control off.
- New root helper `/usr/local/sbin/dcc-component enable|disable fan|cpu|keyboard` / `status` with the polkit action `org.dudes-control-center.component`; the state is the flag file `/etc/dudes-control-center/disabled-<component>`, which the units check with `ConditionPathExists`. The package now depends on `pkexec` (or `policykit-1`).
- Tests for the flag files and package checks for the helper, the polkit action and the unit conditions.

## [1.26.0] - 2026-11-22

### Added
- **Tray menu: Keyboard Profiles**: a submenu below Fan Profiles lists the keyboard profiles, the active one marked "(Active)"; choosing one activates it, like the CPU and Fan submenus. If the Keyboard tab has unsaved changes, the unsaved-changes dialog comes first. The submenu follows every change to the keyboard profiles (create, rename, delete, activate) and appears once the keyboard daemon is connected.

## [1.25.0] - 2026-11-22

### Added
- **Zone editor: drag to mark an area**: while a zone is selected, holding the left mouse button and dragging over the keyboard picture draws a frame; on release every key whose center lies inside joins the zone (and leaves any other zone). If all of them were already in the zone, they leave it instead, like a click on a single key. A click without moving still adds or removes one key.

### Fixed
- A double-click on the keyboard picture counted as three clicks (GTK sends an extra double-click event), which could, for example, make the LED wizard skip an LED. Only real presses count now.
- Tests for the marked area.

## [1.24.4] - 2026-11-22

### Fixed
- **More compact pages, a lower window**: the About page shows title and version in one line, the credits in two columns and the contact in one line, with much less padding. The Logs tab's log views need 150 px instead of 300 px (they still fill the window). The CPU Profiles tab shows the CPU Information in two columns (temperature, load and number of CPUs left; frequencies and power mode right), half the height of the old list.

## [1.24.3] - 2026-11-22

### Fixed
- **Fan Profiles: Cancel button removed**: it only discarded unsaved curve changes, which the unsaved-changes dialog already offers (Discard Changes) when switching profiles, leaving the tab or closing the window.

## [1.24.2] - 2026-11-22

### Fixed
- **Keyboard tab layout**: the LED Wizard (its buttons and status line) moved to the right side of the Backlight section, next to Backlight Off and Brightness; it is the key-to-LED mapping, not part of the zone editor. The keyboard picture is now at the top of the Visual Zone Editor and takes only the height it needs for its width; the Zones list next to it has the same height. The picture used to be centered in all the free space, with empty areas above and below it.

## [1.24.1] - 2026-11-22

### Fixed
- **Activating a keyboard profile without colors** (no base color, no zones) now switches all keys off, the same as its preview. It used to leave the previous profile's colors on the keyboard and log "No zones to apply" as an error.
- **An active profile without colors stays dark** when the GUI closes or leaves the Keyboard tab and when the keyboard daemon starts; it lit up white. White is only used when there is no keyboard profile at all (fresh install).
- Tests for activating, restoring and starting with an empty profile.

### Security
- **Package files belong to root again**: the .deb was built without `--root-owner-group`, so every installed file - including the daemons systemd runs as root, their unit files and the ite_829x source DKMS builds as root - belonged to the user who built the package and was group-writable. That user (or any program running as that user) could replace a root daemon. The package is now built with root ownership and without group/world write; the postinst gives files and directories left by older packages back to root. `make deb-check` fails if any file in the package is not root's or is writable by group or others.

## [1.24.0] - 2026-11-22

### Added
- **Keyboard zone editor**: a keyboard profile is now a **base color** (keys in no zone) plus named **zones** (name, color, keys). A **zone list** next to the keyboard picture shows each zone with its color and number of LEDs and has **Create, Rename, Save, Delete**. While a zone is selected, clicking keys on the picture adds or removes them (a key belongs to one zone) and the preset colors color that zone; with no zone selected they color the whole keyboard (the base color, zones keep their colors), so one-colored profiles need nothing else. **Whole Keyboard** stops editing a zone; with no zone selected, clicking a key selects its zone.
- **Custom colors**: "Custom…" opens a color chooser for any color, for the zone or the whole keyboard.
- **Profile preview**: selecting a keyboard profile in the dropdown shows it on the keyboard and the picture without activating it; leaving the Keyboard tab (or closing the window) brings the active profile back.
- **Unsaved-changes dialog** for keyboard profiles (Cancel / Discard Changes / Save Changes) before switching profiles, Create, Activate, leaving the tab, closing the window or quitting, like the Fan and CPU tabs.
- Keyboard daemon: `PREVIEW` (show a profile's colors without saving), `GET_PROFILE` (a saved profile's base color and zones), `RESTORE_ACTIVE`; profiles.json stores the base color as `"base": {"r","g","b"}`.

### Fixed
- **Save writes the profile you are editing**: it copied the keyboard's current colors (usually the active profile's) into whatever profile was selected, so selecting another profile and saving mixed two profiles; after saving, the dropdown jumped back to the active profile.
- **New keyboard profiles start empty** instead of as a copy of the colors shown at that moment.
- **Deleting a profile before the active one** made the next profile active (the `[*]` moved) while the keyboard kept the old colors.
- The old zone "Base" (whole keyboard color before 1.24.0) is read as the profile's base color. The unconnected "Add Zone" / "Apply Zones" buttons are gone.
- Tests for the zone model, preview, base color, profile reading and the delete fix.

## [1.23.0] - 2026-11-22

### Added
- **CPU Profiles: unsaved-changes dialog**, like the fan curve editor: after changing the core spinners, "Apply values to all cores" or the power mode of a CPU profile, the GUI asks **Cancel / Discard Changes / Save Changes** before switching to another CPU profile, Create, Activate, leaving the CPU tab, closing the window or quitting from the tray. Discard puts the stored values back (and the stored power mode, if the change was already applied to the running profile); Cancel stays where you are.
- Changes are found by comparing the values on screen with the profile as loaded or saved, so values the GUI sets itself never count as changes, and changing a value back is not "unsaved".
- Quitting from the tray now also asks about unsaved fan curve changes (closing the window already did).
- Tests for the change detection (run with a display; skipped and listed as SKIP without one).

## [1.22.2] - 2026-11-22

### Fixed
- **New CPU profiles start in this system's Balanced mode**: the GUI always created them with the governor "schedutil". With intel_pstate in active mode (only performance and powersave), the CPU daemon logged a warning for every new profile and fell back to powersave. The GUI now asks the daemon which governors exist and uses schedutil where it is offered, otherwise powersave with the EPP balance_performance (what the built-in Balanced mode uses there).
- Tests for the choice.

## [1.22.1] - 2026-11-22

### Fixed
- **Same profile buttons in every tab**: the Fan Profiles, CPU and Keyboard tabs now all show **Create, Rename, Save, Delete, Activate**, in that order and with the same short names (before: "New Profile" / "Create Profile" / "Add", "Delete Profile" / "Remove", "Save Profile", "Activate Profile", and three different orders).
- **Fan Profiles: "Save" in the profile row** stores the curve points of the selected profile; it replaces the "Apply" button under the curve. "Cancel" stays there to discard unsaved curve changes, and the unsaved-changes dialog offers "Save Changes".
- Status messages name the new buttons ("click Save to keep it", "click Activate to use it").

## [1.22.0] - 2026-11-22

### Added
- **LED wizard: "No Key" button**: marks the lit LED as lighting no key (some LEDs of the controller have no key) and goes to the next LED. "Skip" still keeps the LED's current key.
- The wizard label shows the key each LED has now ("Click the key for LED 32 / 119 (now: no key)"), so wrong assignments are easy to spot.

### Fixed
- **Wrong keys on the picture during the wizard**: with an empty mapping (the old wizard cleared it when it started) the picture fell back to a built-in one-LED-per-key table with errors: Numpad Entf on LED 32, Numpad Einfg on LED 81, both ß and <> on LED 82, and most of the bottom row shifted. That table is replaced by a built-in LED-to-key mapping in the wizard's own format, taken from a checked wizard run on this keyboard (unused LEDs 32, 55, 73, 81, 107 light no key; Space, Backspace, Shift... have all their LEDs). It is used until a wizard mapping is saved, and nothing falls back to another table anymore.
- A keyboard daemon reconnect during a wizard run no longer reloads the saved mapping over the wizard's unsaved changes.
- Tests for the built-in mapping.

## [1.21.9] - 2026-11-22

### Fixed
- **Logs tab: live keyboard daemon log**: a new "Keyboard Backlight Log" view shows `/var/log/dudes-control-center/KBD_BACKLIGHT_DAEMON.log` like the other logs (newest message on top, updated every 0.5 s while the Logs tab is open).
- The log views are now ordered GUI Log, CPU Daemon Log, Fan Daemon Log, Keyboard Backlight Log.

## [1.21.8] - 2026-11-22

### Fixed
- **LED wizard no longer wipes the mapping**: starting the wizard cleared the whole key-to-LED mapping at once, so "Save & Finish" right after starting wrote an empty mapping, and "Cancel" did not bring it back. The wizard now starts from the existing mapping: each click reassigns only the LED asked for, Skip keeps that LED's key, and Cancel restores the mapping exactly as it was.
- **Numpad Enter on the keyboard picture**: it looked like two buttons and its lower marking went past the right edge. The key table has three entries for Numpad Enter (one per LED plus its lower half), and 1.21.7's L-shape rule took its own lower entry for a neighbour key. All entries of one key (Numpad Enter, Enter, Backspace, Shift, ...) are now drawn and selected as one key, and only a key that really sits to the left makes a key L-shaped.
- Tests for key groups and the Numpad Enter shape.

## [1.21.7] - 2026-11-22

### Fixed
- **Keyboard picture: the Enter key no longer covers the '#' key**. Enter spans two rows and was stored as one rectangle, so its colored/selected overlay reached into the '#' key, and clicking that part of '#' selected Enter. Keys spanning two rows are now L-shaped where a key sits beside their lower half (ISO Enter next to '#'), for drawing and clicking; other tall keys (Numpad +, Numpad Enter) stay rectangles.
- Selected keys get their green outline again (the outline was drawn after the fill had already used up the shape, so it never appeared).
- Test for the key shapes.

## [1.21.6] - 2026-11-22

### Fixed
- The daemon status lines of the Fan Profiles and CPU Profiles tabs were 8 pixels lower than the Keyboard tab's; all three are at the same height now (12 px top margin in all three tabs).

## [1.21.5] - 2026-11-22

### Fixed
- Status tab: removed the "Fan Status & Manual Control" heading at the top (it had no function and only took space; no other tab has one).

## [1.21.4] - 2026-11-22

### Fixed
- Fan Profiles tab: the "Apply Changes" button is now called **Apply** and sits next to **Cancel** instead of at the far right edge.

## [1.21.3] - 2026-11-22

### Fixed
- **Keyboard tab is more compact**: above the keyboard picture there was an always-empty 50-pixel area (reserved for zone editors that are not built yet) and an empty LED wizard status line. Both took space and made the window taller than needed. The area is no longer shown (it stays reserved for the coming zone configuration with "Add Zone" / "Apply Zones"), and the wizard status line only appears while the LED wizard is used.

## [1.21.2] - 2026-11-22

### Fixed
- Fan Profiles tab: removed the "Fan Profiles" heading at the top (it repeated the tab name and only took space).

## [1.21.1] - 2026-11-22

### Fixed
- **Daemon status in every tab**: the Fan Profiles and CPU Profiles tabs now show the connection to their daemon at the top, like the Keyboard tab ("Fan Daemon Status", "CPU Daemon Status", "Keyboard Daemon Status"). A disconnected daemon is shown in **red** ("... daemon not connected"); the lines update every 2 seconds.
- **The GUI reconnects to the CPU daemon**: after the CPU daemon was restarted or updated, the CPU tab stayed disconnected until the GUI was reopened (the keyboard connection had the same problem until 1.18.6). The CPU refresh timer now reconnects and reloads the power modes, core count, hard limit and profiles. It also runs when the GUI is started while the CPU daemon is not running (before, the timer was only started if the daemon was reachable at startup).

## [1.21.0] - 2026-11-22

### Changed
- **Curve Editor and Fan Profiles are one tab: "Fan Profiles"**. A single dropdown lists all fan profiles, with the one in use marked "(Active)". Next to it: **Activate** (makes the selected profile control the fans), New, Rename and Delete. Below: the curve points and graph of the selected profile with Add/Delete Point and Apply Changes/Cancel, as before. The radio list with its separate Apply/Cancel buttons is gone, so there is only one place to pick a profile. The manual mode warning and "Disable Manual Mode" moved into this tab; Activate is disabled in manual mode.
- Refreshing the list keeps the selected profile instead of jumping to the active one (as in the CPU tab since 1.20.2); Activate asks about unsaved curve changes first.

### Fixed
- **Logs tab refreshes again**: its auto-refresh checked for the wrong tab number since the Keyboard tab was added, so the logs only refreshed while the Keyboard tab was open. Tab numbers are now named constants.

## [1.20.4] - 2026-11-22

### Fixed
- **Choosing a power mode for the active profile applies it right away**: since 1.17.1 the power mode list only edited the loaded profile (stored on Save, applied on Activate). Because a profile is always loaded, choosing a mode never changed the CPU. Now, if the loaded profile is the active one, the mode is applied immediately (governor and energy preference only; the profile's frequency limits stay) and the profile is marked as changed - **Save** keeps the mode in the profile. For a profile that is not active, choosing a mode still only edits it.
- New CPU daemon command `SET_POWER_MODE|<mode>`: sets only governor and EPP of a power mode. `APPLY_GOVERNOR` also resets every core's maximum frequency to the hard limit, which would override the active profile's per-core limits.
- Tests for `SET_POWER_MODE` (8 suites, 194 checks).

## [1.20.3] - 2026-11-22

### Fixed
- **CPU tab shows the power mode the CPU really runs**: "Active Governor" always said "Balanced", because the CPU daemon reported its internal default mode (set at startup) instead of what was applied by a profile or the power mode list. It now reads cpu0's governor and energy preference from the system and shows the matching mode with the raw values, e.g. `Performance (performance)` or `Balanced (powersave, balance_performance)`. The label is now "Active Power Mode".
- The CPU daemon's `STATUS` reports the real mode in `ACTIVE_GOVERNOR` and adds `GOVERNOR` and `EPP`; `dcc-cpu-cli` shows the real mode too.
- Tests for the reported mode after switching modes (8 suites, 192 checks).

## [1.20.2] - 2026-11-22

### Fixed
- **CPU tab no longer jumps to the active profile after saving**: reloading the profile list always selected the active profile (`[*]`) and loaded its values into the core grid; after saving, only the dropdown was switched back, so the tab showed (or briefly showed) another profile, and the dropdown and the core values could disagree. The list reload now takes the profile to stay on: saving and renaming keep the edited profile, switching back to the CPU tab keeps the profile you had selected (the active one on the first visit).

## [1.20.1] - 2026-11-22

### Fixed
- **CPU tab (and keyboard actions) much faster**: the GUI could not tell where a reply from the CPU or keyboard daemon ended, so after every reply it waited until the connection had been quiet for 200 ms. Loading a CPU profile sends 4-6 commands and saving more, which added up to a second or more; each key color change cost about 0.6 s. The daemons now end every reply with a NUL byte as end marker and the GUI stops reading there: a command takes about 0.1 ms instead of 200 ms. The GUI also waits up to 3 s for a slow command instead of giving up after 200 ms (which could leave a late reply to be read as the answer to the next command), and still works with older daemons that send no marker.
- Test: CPU command round trip under 50 ms (measured 0.1 ms) (8 suites, 189 checks).

## [1.20.0] - 2026-11-22

### Added
- **Keyboard brightness slider** in the Keyboard tab (0-10). It sets the keyboard's hardware brightness, on top of the per-key colors.
- **The desktop's keyboard brightness slider works** (e.g. Linux Mint's battery/power applet, through UPower): it changes the same brightness. The GUI slider follows it within a few seconds, and the keyboard daemon remembers the level from either slider for the next start.
- Keyboard daemon: `SET_BRIGHTNESS <0-10>`, `GET_BRIGHTNESS` (`BRIGHTNESS <level> MAX <max>`), `STATUS` reports `BRIGHTNESS` and `BRIGHTNESS_MAX`; the level is stored in `/var/lib/dudes-control-center/kbd_brightness`.

### Changed
- **Keyboard driver `ite_829x`: hardware brightness restored as in tuxedo-drivers**. The modified driver reported a range of 0-255 but always sent full brightness to the keyboard, so writing the brightness (what the desktop slider does) had no effect. It now passes the brightness (0-10, `max_brightness` 10) to the keyboard again. Unlike tuxedo-drivers it starts at full brightness instead of 0. One key's brightness applies to the whole keyboard, so desktop sliders that control a single LED device work.
- The keyboard daemon no longer dims by scaling the colors; brightness is the hardware setting.
- **After upgrading from an older version, log out and in once**: the desktop's power daemon (Cinnamon's `csd-power`) reads the keyboard's brightness range at login and otherwise keeps using the old 0-255 range, so its slider only switches between off and full.
- `postinst` restarts UPower after loading the rebuilt driver, because the keyboard LED devices are recreated and UPower would keep using the old ones (the desktop slider then failed with "error reading brightness").
- GUI: the "Backlight Power" frame is now "Backlight" (brightness and off switch).
- Tests: brightness commands, limits, a change from outside (desktop slider), restore after a restart; package check for the UPower restart (8 suites, 187 checks).

### Documentation
- `LICENSE` contains only the GPL version 2 text, so GitHub recognizes the license; the copyright statement and the third-party origins are in `COPYRIGHT.md`.


### Documentation
- **`LICENSE` holds only the GPL version 2 text**; the license statement and the origins of the third-party code moved to `COPYRIGHT.md`.
## [1.19.4] - 2026-11-22

### Fixed
- **The project has a license: GNU GPL version 2 (GPL-2.0-only)**. The fan control code (Embedded Controller access) was imported from [fan-control](https://github.com/steiiin/fan-control) by steiiin, which has no license file but is a fork of [clevo-fan-control](https://github.com/agramian/clevo-fan-control) by agramian, licensed GPL-2.0 (itself based on Clevo Fan Control Indicator by AqD and fanctrl.c by Jonas Diemer). Code derived from it must stay GPL-2.0, so the whole project is now GPL-2.0. The README previously only said to "refer to the original project for its license terms".
  - `LICENSE` at the top of the repository: license statement, third-party origins (fan code chain, TUXEDO keyboard driver; the fan curves and the curve editor are the project's own work) and the full GPL version 2 text.
  - `SPDX-License-Identifier: GPL-2.0-only` in all source files, scripts, tests and maintainer scripts; `src/dcc-common.c` names the origin of the EC code. The TUXEDO driver keeps its GPL-2.0-or-later header.
  - The package installs a machine-readable `/usr/share/doc/dudes-control-center/copyright` (project GPL-2.0-only with the fan code origin, driver GPL-2.0+ by TUXEDO Computers) and `LICENSE`.
- **Credits**: README and the About page credit the whole fan code chain (steiiin, agramian, AqD, Jonas Diemer) instead of only fan-control, and the About page states the license.
- The package check verifies the copyright file and the installed license.

## [1.19.3] - 2026-11-22

### Fixed
- **Correct attribution of the keyboard driver**: `drivers/ite_829x.c` was presented as written by this project ("Dudes Keyboard LED Control", also as `MODULE_AUTHOR`). It is the ITE 829x driver from **[tuxedo-drivers](https://github.com/tuxedocomputers/tuxedo-drivers)** by **TUXEDO Computers GmbH** (copyright 2019-2023, GPL-2.0-or-later), modified for this project. The source header carries TUXEDO's copyright again and says where it comes from, `MODULE_AUTHOR` is TUXEDO Computers GmbH (shown by `modinfo ite_829x`), and the description names tuxedo-drivers.
- **License**: `drivers/LICENSE` states origin, copyright and GPL-2.0-or-later with the full GPL version 2 text; `drivers/README.md` explains the origin. The package installs the license with the driver source (`/usr/src/ite_829x-<version>/LICENSE`) and as `/usr/share/doc/dudes-control-center/copyright`.
- **Credits**: README (components, hardware, thanks, license, references) and the GUI's About page credit TUXEDO Computers for the driver, with a link to tuxedo-drivers.
- The package check verifies that the TUXEDO copyright and the GPL text are shipped with the driver.

## [1.19.2] - 2026-11-22

### Fixed
- **Fan percentage no longer computed from the RPM**: the GUI and CLI calculated it with fixed maximums measured on one laptop (6022 RPM CPU fan, 5859 RPM GPU fan). That only fits those fans (other models, dust and age change the maximum), RPM is not proportional to the duty, and the GUI rounded differently from the CLI. The percentage is now the fan duty, shown next to the measured RPM: `CPU Fan: 3500 RPM (54%)`.
- **Where the duty comes from**: the EC's duty registers (`0xCE`/`0xCF`) where the EC keeps them up to date. Some ECs - including the test laptop, which takes fan speeds through EC command `0x99` - leave these registers at 0 while the fans spin; a 0 at 300 RPM or more is therefore not believed, and the fan daemon reports the duty it set instead (logged once). The CLI, which reads the EC directly, shows only the RPM in that case.
- The fan daemon's `STATUS` reports `CPU_FAN_DUTY` and `GPU_FAN_DUTY`; the GUI and CLI format both fans the same way. The fixed maximum RPM values and the GUI's duplicate formula are removed.
- Tests for the fan text, the plausibility check and the new `STATUS` fields (8 suites, 178 checks).

## [1.19.1] - 2026-11-22

### Fixed
- **GUI crashed when a link on the About page was clicked twice**: the link's URL was freed on every left click although it belongs to the link, so the next click used freed memory and freed it again (glibc aborted the GUI with "double free"). Clicking quickly made it likely. The URL is now freed once, when the link widget is destroyed. Only a single left press opens the link, so a double click no longer opens two browser tabs.
- Test: repeated clicks on a link under AddressSanitizer (8 suites, 172 checks).

## [1.19.0] - 2026-11-22

### Changed
- **Short names for commands and services: `dcc-`** instead of `dudes-control-center-`. The application is still called Dudes Control Center.

  | Before | Now |
  |---|---|
  | `dudes-control-center-gui` | `dcc-gui` |
  | `dudes-control-center-fan-cli` / `-cpu-cli` | `dcc-fan-cli` / `dcc-cpu-cli` |
  | `dudes-control-center-fan-daemon` / `-cpu-daemon` (programs and services) | `dcc-fan-daemon` / `dcc-cpu-daemon` |
  | `dudes-control-center-kbd-backlight-daemon` (program and service) | `dcc-kbd-daemon` |
  | `dudes-control-center-hard-limit-enforcer` (service and timer), `-enforce` (script) | `dcc-hard-limit-enforcer`, `dcc-hard-limit-enforce` |
  | sockets `/run/dudes-control-center-{fan,cpu,kbd}.sock` | `/run/dcc-{fan,cpu,kbd}.sock` |
  | source files `src/dudes-control-center-*` | `src/dcc-*` |

  **Unchanged** (data and identity): `~/.config/dudes-control-center/`, `~/.cache/dudes-control-center/`, `/etc/dudes-control-center/`, `/var/lib/dudes-control-center/`, `/var/log/dudes-control-center/`, the `dudes-control-center` group, the package name, the menu entry and icon.
- **Upgrade**: a new `preinst` stops and disables the old services while their files still exist (the fan service's fail-safe sets the fans to 100% until the new daemon runs); `postinst` removes what is left of them and starts the new services. The old commands `dudes-control-center-gui`, `-fan-cli` and `-cpu-cli` stay as links to the new ones for now, so autostart entries and scripts keep working.
- **Packaging**: the maintainer scripts are real files in `packaging/` (version and group filled in by `make deb`) instead of long `printf` lines in the Makefile.

### Added
- **Package check** `make deb-check` (`tests/check_deb.sh`, run in CI): all programs and units present, every service and the menu entry start a program that is in the package, no leftover old names except the compatibility links, maintainer scripts valid and complete (33 checks).

### Fixed
- CPU daemon: the intended 31-character limit of the command word is written into the format (`%.31s`), so the test build has no warning either. No change in behavior.

## [1.18.8] - 2026-11-22

### Fixed
- **No compiler warnings**: the 22 remaining warnings of a full build are fixed:
  - Possible text truncation: CPU daemon command word, per-core frequency labels in the GUI, LED paths in the keyboard daemon. Bounded copies or dynamically sized strings now.
  - Keyboard zone list: the write position mixed signed and unsigned sizes and could step past the end of the buffer if `snprintf` truncated. It is clamped now.
  - Deprecated GTK stock buttons in the Keyboard tab replaced by "Add" / "Remove" labels.
  - Unused variables, parameters and functions removed (including the never-used `disconnect_from_keyboard_daemon` and `condition_type_from_string`), and an always-false brightness check.
- **CI fails on new warnings**: GitHub Actions builds with `-Werror` (`make all EXTRA_CFLAGS=-Werror`).

## [1.18.7] - 2026-11-22

### Fixed
- **Keyboard LEDs are numbered by name**: the keyboard daemon numbered the LEDs in the order `/sys/class/leds` lists them (a hash order: LED 0 was `rgb:kbd_backlight_59`), and the LED wizard mapping and keyboard profiles depended on that order. Now LED N is `rgb:kbd_backlight_N` (the base `rgb:kbd_backlight` is 0), which also follows the physical key order (Esc = 0, F1 = 1, ...).
- **Existing data is converted once**: on its first start the daemon converts keyboard profiles (`"led_numbering": "name"` in `profiles.json`) and the LED wizard mapping (`numbering=name` in the CSV header) from the old numbering, so the wizard does not need to be run again. The GUI's built-in key table uses the new numbering too.
- Tests for the numbering and the one-time conversion (8 suites, 171 checks).

## [1.18.6] - 2026-11-22

### Fixed
- **Opening the GUI no longer turns the keyboard backlight on**: the GUI sent `TURN_ON` at every start, which undid "Backlight Off" and, before 1.18.0, replaced the colors with white. The GUI now only reads the daemon's state and shows it on the **Backlight Off** switch.
- **"Backlight Off" survives restarts and reboots**: the keyboard daemon remembers it (`/var/lib/dudes-control-center/kbd_backlight_off`) and stays off when it starts. Otherwise it restores the active profile's colors, or white if the profile has none (this used to depend on the GUI being opened).
- The Backlight Off switch follows the real state: choosing a color, activating a profile with colors or finishing the LED wizard switches the light on, and the switch now shows that. `STATUS` reports `IS_ON=1` after a restored profile.
- **The GUI reconnects to the keyboard daemon**: after the daemon was restarted or updated, the GUI stayed disconnected and silently skipped keyboard actions ("not connected, skipping"). It now reconnects when a keyboard action finds the connection gone (and resends the command), and checks the connection every 6 s, which also keeps the Backlight Off switch in sync.
- GUI commands to the daemons no longer risk a `SIGPIPE` when a daemon has gone away (`send` with `MSG_NOSIGNAL`).
- The keyboard daemon logs read-only polling commands (`STATUS`, `GET_*`, `LIST_*`, `PING`) only at DEBUG level.
- `apt purge` also removes `/var/lib/dudes-control-center`.
- Tests for the off state, start-up lighting and the GUI reconnect (8 suites, 167 checks).

### Documentation (since 1.18.5)
- README rewritten for the current state: all components (CPU and keyboard daemons, `ite_829x` DKMS module, hard-limit enforcer), installation from the release `.deb`, the GUI tabs, per-key lighting and the LED wizard, CPU power modes, manual fan mode down to 0% and the emergency handover, configuration and which user's `profiles.json` the daemons use, log files, building and testing, troubleshooting. Removed outdated parts (version 1.0 badge, GTK4, 10% minimum, old log names, running daemons by hand).


### Documentation
- **README rewritten** for the current state of the project (components, installation, GUI tabs, safety features, configuration, troubleshooting).
## [1.18.5] - 2026-11-22

### Fixed
- **Keyboard daemon serves several clients at once**: every new connection closed the previous one, so the CLI, a script or a second GUI window disconnected the GUI from the keyboard daemon (the CPU daemon had the same bug until 1.16.4). Up to 10 clients are served now.
- **Keyboard commands are newline-framed**: a command that arrived in two pieces was lost or cut in half - more likely since commands can be up to 4 KB (1.18.0). Commands now run once their newline arrives; a command without a newline runs after a 200 ms pause, as before. A client that stops reading responses is disconnected instead of blocking the daemon.
- Test suite covers both (8 suites, 158 checks).

## [1.18.4] - 2026-11-22

### Fixed
- **No heat cycle after an emergency in manual mode**: when the 95°C emergency override ended, the fan daemon went back to the manual speed. At a low speed such as 0% the CPU heated up to 95°C again (on the test laptop 73 -> 95°C in about 80 s), so the fans would cycle between off and full speed. Now an emergency ends manual mode as soon as it starts: the fans run at 100% until it clears (below 90°C), then follow the fan curve (auto mode). The GUI switches to auto and says why at the moment the fans speed up. A manual speed set during the emergency also hands over to auto when it clears. After a sensor failure (not overheating) the manual speed is still restored.
- **Manual and auto mode changes are logged by the fan daemon** (`Manual mode: fan speed N%`, `Auto mode on/off`, and the emergency handover), so the daemon log shows what the fans were told to do.
- Fan daemon `STATUS` reports `EMERGENCY_AUTO=<count>`.
- Tests for the handover (8 suites, 153 checks).

## [1.18.3] - 2026-11-22

### Fixed
- **Manual fan mode can go down to 0% (fans off)**: the GUI's minus button stopped at 10%, although the fan daemon and the CLI already accept 0%. The fail-safes still apply at 0%: the fans switch to 100% at 95°C or when a temperature sensor fails, and back to the manual value when it clears. The status line says so when the fans are stopped.
- Manual mode: the speed shown when switching it on now has a `%` sign like the +/- buttons; an odd speed (e.g. 37%) steps down to the next 10%.
- CLI help: `--set` accepts 0,10,...,100.
- Tests for the manual speed steps (8 suites, 149 checks).

## [1.18.2] - 2026-11-22

### Fixed
- **CPU daemon loaded its profiles twice at startup**: `cpu_control_init()` and the daemon's `main()` both loaded the custom profiles and the hard limit, so every message and warning about them (e.g. an unavailable governor) appeared twice in the log. They are loaded once now; SIGHUP still reloads them.
- **False "Could not load saved profiles, using defaults" warning at every start**: the power modes (Performance / Balanced / Power Save) are built in and never loaded from disk, but a leftover stub always reported a failed load. Removed; the log now says how many built-in power modes are available.
- Tests check the startup log for both (8 suites, 144 checks).

## [1.18.1] - 2026-11-22

### Fixed
- **No more hard-coded `/home/dude`**: the fan, CPU and keyboard daemons and the hard-limit enforcer read `profiles.json` from `/home/dude/.config/...`, so the app only worked for a user named `dude`. They now find the desktop user like this (first match wins):
  1. `DCC_PROFILES_PATH` (absolute path, e.g. in a systemd drop-in),
  2. the user in `/etc/dudes-control-center/user`, written by the package when it is installed with `sudo` (kept on upgrades, removed on purge),
  3. the first member of the `dudes-control-center` group,
  4. the user with uid 1000.

  Root and unknown users in the user file are refused. The chosen path is logged once (`[CONFIG] Using ...`).
- The hard-limit enforcer service no longer sets `HOME=/home/dude`.
- The tray icon's development fallback looks next to the executable instead of an old project path; `restart-daemon.sh` uses its own folder.
- Test suite: lookup of the profiles path (8 suites, 140 checks).

## [1.18.0] - 2026-11-22

### Added
- **Per-key keyboard lighting**: select keys on the keyboard picture in the Keyboard tab, then click a color - exactly those keys light up in that color and the other keys keep theirs. With no keys selected, a color applies to the whole keyboard (as before). **Save Profile** stores the per-key colors in the keyboard profile, **Activate Profile** brings them back. A new **Clear selection** button deselects all keys.
- The keyboard picture shows every key in its current color.
- The keyboard daemon restores the active profile's colors when it starts (after a reboot or update), and turning the backlight back on restores the per-key colors instead of switching to plain white.
- New daemon command `SET_KEYS_COLOR::<R,G,B>::<led,led,...|all>`: keys of the same color share one zone (named after the color), keys leave their old zone when recolored, and zones left empty are removed.
- Test suite: end-to-end test of the keyboard daemon against a fake 120-LED `/sys/class/leds` (7 suites, 132 checks). Socket, LED directory, log directory and profiles path can be overridden at build time.

### Fixed
- **Keyboard commands are no longer limited to 256 bytes**: the daemon cut longer commands off, so a list of many LEDs (zones, per-key colors) lost keys. Commands up to 4 KB are accepted now (the GUI side had the same limit).
- **Keyboard daemon could hang on shutdown**: its signal handlers wrote to the log, which is not safe inside a signal handler and could deadlock the daemon when stopped (found by the new test). They only set flags now, as in the CPU daemon since 1.16.4.
- A keyboard profile without colors can be saved (it was refused with "No zones to save").
- Loading keyboard profiles: zones and key counts beyond the limits are skipped instead of leaving gaps.

### Note
- Clicking a key in 1.17.3 only changed the selection in the GUI; nothing was sent to the keyboard. The 1.17.3 entry is corrected below.

## [1.17.3] - 2026-11-22

### Fixed
- **Keyboard tab: clicking a key now selects the right LEDs in the GUI** (the selection was not sent to the keyboard yet; per-key lighting came in 1.18.0): the canvas always used a built-in key-to-LED table, even after the LED wizard had saved your own mapping (`keyboard_led_mapping.csv`). For 29 keys the table pointed at a different LED than the wizard mapping, and keys lit by several LEDs (Space, Enter, Backspace, ...) selected only one. Clicks and highlights now use the wizard mapping when it exists (all LEDs of a key; clicking again deselects them) and the built-in table only without one.
- **Key tables were inconsistent**: the built-in LED table had 122 values for 120 slots (the compiler dropped two), while there are only 113 key names and positions; the last 7 "keys" were empty entries. All three tables now have exactly 113 entries (`KBD_NUM_KEYS`).
- **Rename keyboard profile**: the "name already exists" error dialog was passed to `gtk_dialog_run` with the wrong type.
- **`ite_829x` reports its version**: the module had no `MODULE_VERSION`, so `modinfo` and the install output showed none. "make deb" now builds it with the package version.
- **CI**: `actions/upload-artifact` v5 -> v6 (v5 still targets the deprecated Node.js 20).
- Tests cover the keyboard key/LED lookup (110 checks).

## [1.17.2] - 2026-11-22

### Fixed
- **Build outputs are no longer tracked in git**: `dudes-control-center-cpu-cli` and `dudes-control-center-kbd-backlight-daemon` were committed binaries, so every build showed them as modified; old `.deb` packages (1.8.0-1.13.1) were committed in `debs/`. They are removed from the repository (local files are kept) and `.gitignore` now covers all six binaries, `debs/` and `*.deb` (tested packages are attached to GitHub releases), Python bytecode from the tests and kernel module build output in `drivers/`. Older commits still contain them; the history is not rewritten.
- **CI**: `actions/checkout` and `actions/upload-artifact` updated from v4 to v5 (v4 runs on the deprecated Node.js 20).

## [1.17.1] - 2026-11-22

### Fixed
- **Power modes work with `intel_pstate` (Balanced restored)**: the CPU daemon offered Performance, Balanced (`schedutil`) and Power Save, but `intel_pstate` in active mode (common on Intel laptops) only has the `performance` and `powersave` governors. Choosing "Balanced" failed silently, and profiles saved with it stored `schedutil`, so their governor never applied. A power mode is now a governor plus the CPU's energy performance preference (EPP), like the power profiles of Mint/GNOME/Windows:
  - **Performance**: `performance` governor.
  - **Balanced**: `schedutil` where available, otherwise `powersave` + EPP `balance_performance`.
  - **Power Save**: `powersave` + EPP `power`.

  Modes are only offered when the system supports them. EPP is stored per policy and per profile (`"epp"` in `profiles.json`, `governor:epp` in the protocol).
- **The CPU daemon no longer forces full speed**: it set intel_pstate `min_perf_pct = 100` at every start (keeping every core near its maximum frequency regardless of load), and forced EPP `performance` at startup, on every profile apply and on every hard-limit change. That made Balanced and Power Save meaningless. EPP is now set by the active power mode only, and a `min_perf_pct` of 100 left behind by older versions is reset once to the driver's minimum.
- **Profiles from older versions keep working**: a profile naming an unavailable governor uses the closest available mode (`schedutil` → Balanced, `powersave` → Power Save), with a warning at startup and on apply. Saving stores only available governors/EPPs, so profiles are corrected the next time they are saved.
- **GUI**: the "Select Profile" list is now "Power mode". With a per-core profile loaded it shows that profile's mode and changes it for that profile (stored on Save, applied on Activate) instead of switching the whole system immediately.
- Tests cover power modes, EPP handling, `min_perf_pct` reset and the GUI mode matching (104 checks).

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

## [1.9.1] - 2026-11-20

### Fixed
- **Redundant `is_active` marker removed from fan profiles**: the active fan profile is stored once as `active_profile_index`; the per-profile marker could disagree with it.

## [1.9.0] - 2026-11-20

### Added
- **Batch `SAVE_CUSTOM_PROFILE` command**: the CPU daemon saves all per-core frequencies of a profile in one command instead of one command per core, which makes saving a profile much faster.

### Fixed
- **Active profile not persisted**: activating a fan or CPU profile now stores it as the active profile, so it is used again after a restart.
- **Save button stuck**: the Save button no longer stays pressed while a profile is being saved.

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

## [1.5.1] - 2026-11-17

### Fixed
- **CPU daemon log lines**: commands are logged with their non-empty arguments only (no duplicate or trailing empty ones).
- **Logs tab reorganized**: the log sections stacked on top of each other became sub-tabs (GUI Log, Fan Daemon Log, CPU Daemon Log), which saves vertical space and leaves room for more daemon logs.

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

[Unreleased]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.34.2...HEAD
[1.34.2]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.34.1...v1.34.2
[1.34.1]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.34.0...v1.34.1
[1.34.0]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.33.0...v1.34.0
[1.33.0]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.32.1...v1.33.0
[1.32.1]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.32.0...v1.32.1
[1.32.0]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.31.0...v1.32.0
[1.31.0]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.30.0...v1.31.0
[1.30.0]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.29.1...v1.30.0
[1.29.1]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.29.0...v1.29.1
[1.29.0]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.28.0...v1.29.0
[1.28.0]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.27.0...v1.28.0
[1.27.0]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.26.0...v1.27.0
[1.26.0]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.25.0...v1.26.0
[1.25.0]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.24.4...v1.25.0
[1.24.4]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.24.3...v1.24.4
[1.24.3]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.24.2...v1.24.3
[1.24.2]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.24.1...v1.24.2
[1.24.1]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.24.0...v1.24.1
[1.24.0]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.23.0...v1.24.0
[1.23.0]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.22.2...v1.23.0
[1.22.2]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.22.1...v1.22.2
[1.22.1]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.22.0...v1.22.1
[1.22.0]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.21.9...v1.22.0
[1.21.9]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.21.8...v1.21.9
[1.21.8]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.21.7...v1.21.8
[1.21.7]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.21.6...v1.21.7
[1.21.6]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.21.5...v1.21.6
[1.21.5]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.21.4...v1.21.5
[1.21.4]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.21.3...v1.21.4
[1.21.3]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.21.2...v1.21.3
[1.21.2]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.21.1...v1.21.2
[1.21.1]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.21.0...v1.21.1
[1.21.0]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.20.4...v1.21.0
[1.20.4]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.20.3...v1.20.4
[1.20.3]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.20.2...v1.20.3
[1.20.2]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.20.1...v1.20.2
[1.20.1]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.20.0...v1.20.1
[1.20.0]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.19.4...v1.20.0
[1.19.4]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.19.3...v1.19.4
[1.19.3]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.19.2...v1.19.3
[1.19.2]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.19.1...v1.19.2
[1.19.1]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.19.0...v1.19.1
[1.19.0]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.18.8...v1.19.0
[1.18.8]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.18.7...v1.18.8
[1.18.7]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.18.6...v1.18.7
[1.18.6]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.18.5...v1.18.6
[1.18.5]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.18.4...v1.18.5
[1.18.4]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.18.3...v1.18.4
[1.18.3]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.18.2...v1.18.3
[1.18.2]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.18.1...v1.18.2
[1.18.1]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.18.0...v1.18.1
[1.18.0]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.17.3...v1.18.0
[1.17.3]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.17.2...v1.17.3
[1.17.2]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.17.1...v1.17.2
[1.17.1]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.17.0...v1.17.1
[1.17.0]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.16.4...v1.17.0
[1.16.4]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.16.3...v1.16.4
[1.16.3]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.16.2...v1.16.3
[1.16.2]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.16.1...v1.16.2
[1.16.1]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.16.0...v1.16.1
[1.16.0]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.15.0...v1.16.0
[1.15.0]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.13.0...v1.15.0
[1.13.0]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.12.0...v1.13.0
[1.12.0]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.11.0...v1.12.0
[1.11.0]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.10.0...v1.11.0
[1.10.0]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.9.0...v1.10.0
[1.9.0]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.8.0...v1.9.0
[1.8.0]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.7.0...v1.8.0
[1.7.0]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.6.7...v1.7.0
[1.6.7]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.6.6...v1.6.7
[1.6.6]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.6.5...v1.6.6
[1.6.5]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.6.4...v1.6.5
[1.6.4]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.6.3...v1.6.4
[1.6.3]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.6.2...v1.6.3
[1.6.2]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.6.1...v1.6.2
[1.6.1]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.6.0...v1.6.1
[1.6.0]: https://github.com/dudewayne/Dudes-Control-Center/compare/v1.5.1...v1.6.0
[1.5.1]: https://github.com/dudewayne/Dudes-Control-Center/releases/tag/v1.5.1
