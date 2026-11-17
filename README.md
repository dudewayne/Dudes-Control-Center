# Dudes Control Center

A comprehensive suite of applications for controlling notebook fans on Linux, specifically designed for Clevo-based notebooks using EC (Embedded Controller) I/O port access. This application was completely coded by AI with [Bionic AI Agent](https://lmstudio.ai/).

![Version](https://img.shields.io/badge/version-1.0-blue.svg)
![License](https://img.shields.io/badge/license-MIT-green.svg)
![Platform](https://img.shields.io/badge/platform-Linux-lightgrey.svg)

## Applications Overview

### 1. dudes-control-center-fan-cli
Command-line interface for reading temperatures and controlling fan speed.

**Features:**
- Read CPU temperature (Intel Ultra 9 and other CPUs)
- Read GPU temperature (NVIDIA RTX 5090 Mobile and other GPUs)
- Read fan RPM and calculate percentage (~1130 RPM = 10%)
- Set manual fan speed in 10% steps (10%, 20%, ..., 100%)
- Auto mode for automatic temperature-based control
- Real-time status display

**Usage:**
```bash
# Show current status (default)
sudo ./dudes-control-center-fan-cli

# Set fan to specific speed (10, 20, ..., 100%)
sudo ./dudes-control-center-fan-cli --set 50

# Enable automatic temperature-based control
sudo ./dudes-control-center-fan-cli --auto

# Show help
./dudes-control-center-fan-cli --help
```

### 2. dudes-control-center-fan-daemon
Background daemon that runs as a systemd service, providing IPC via Unix socket for the GUI application. Includes safety features like watchdog timer and fail-safe mode.

**Features:**
- Runs as background service (foreground or systemd)
- Provides temperature and fan status monitoring
- Supports multiple fan control profiles with custom temperature curves
- Communicates via Unix socket at `/run/dudes-control-center-fan.sock`
- **Watchdog Timer**: 10-second timeout triggers fail-safe mode
- **Fail-Safe Mode**: Automatically sets fans to 100% on errors or unresponsive main loop
- **Logging System**: Comprehensive logging with rotation (5 files, 10MB each)
- Profile persistence to `~/.config/dudes-control-center/profiles.json`

**Usage:**
```bash
# Run in foreground (for debugging)
sudo ./dudes-control-center-fan-daemon --foreground

# Or install as systemd service:
sudo make install
sudo systemctl start dudes-control-center-fan-daemon
sudo systemctl enable dudes-control-center-fan-daemon
```

### 3. dudes-control-center-gui
Full-featured GTK4 graphical interface for fan control with profile management, real-time monitoring, and system tray integration.

**Features:**

#### Tab 1: Status Monitor
- Real-time CPU temperature display
- Real-time GPU temperature display
- Fan speed in RPM and percentage
- Current active profile indicator
- Auto/Manual mode status

#### Tab 2: Curve Editor
- **Graphical Temperature Curve Visualization**: Cairo-based graph showing temperature vs. fan speed curve
- Add, edit, and delete temperature-fan curve points
- Profile selection combo box
- Enter key to quickly add curve points
- Unsaved changes detection with Apply/Discard dialog when switching tabs
- Real-time curve updates as you modify points

#### Tab 3: Profiles Management
- Create new fan control profiles
- Rename existing profiles
- Delete unwanted profiles
- Radio button selection for profile activation
- Manual mode warning indicator (shows when daemon is in manual control)
- Profile list refresh on tab switch

#### Tab 4: Logs Viewer
- **Live GUI logs**: Real-time display of GUI application log output
- **Live Daemon logs**: Real-time display of daemon log output
- Auto-refresh every 500ms when on the Logs tab
- Separate scrolled windows for GUI and daemon logs
- Bold section headers for easy navigation

#### Tab 5: About Page
- Application version (1.0)
- AI-coded information with clickable link to [LM Studio](https://lmstudio.ai/)
- Thank you messages with clickable links:
  - [Original fan-control creator](https://github.com/steiiin/fan-control)
  - [Linus Torvalds](https://torvalds-family.blogspot.com/)
  - The Great Linux Community
  - [Clevo](https://www.clevo.com.tw/) for awesome laptops
- Contact information:
  - Dudemeister
  - Email: wayne.dude@proton.me (clickable mailto link)
  - GitHub: [dudewayne](https://github.com/dudewayne) (clickable link)

#### System Tray Integration
- **Minimize to tray**: Close button minimizes to system tray instead of quitting
- **Left-click toggle**: Click tray icon to show/hide window
- **Right-click context menu**:
  - Profile selection submenu
  - Version label (greyed out with separators)
  - "Show Application" menu item
  - Exit option
- Starts minimized to tray by default

#### Window Management
- **Position saving**: Remembers window position and size between sessions
- **Center on first run**: If no saved position, centers window on screen
- **Single instance check**: Prevents multiple GUI instances via PID file (`~/.config/dudes-control-center/gui.pid`)

**Usage:**
```bash
# First start the daemon
sudo ./dudes-control-center-fan-daemon --foreground &

# Then run the GUI
./dudes-control-center-gui
```

## Building

### Prerequisites
```bash
# Install build dependencies (Debian/Ubuntu)
sudo apt install build-essential libgtk-3-dev pkg-config

# For Fedora/RHEL:
sudo dnf install gcc gtk3-devel pkgconf-pkgconfig

# For Arch Linux:
sudo pacman -S base-devel gtk3 pkgconf
```

### Dependencies
**Compile-time:**
- GCC or Clang compiler
- GTK+ 3.24 or higher (for GUI)
- pkg-config
- Make

**Runtime:**
- GTK+ 3.24 libraries
- Linux kernel with I/O port access support
- Root/sudo privileges for daemon and CLI

### Build All Applications
```bash
make all
```

### Build Individual Applications
```bash
make cli      # Build CLI only
make daemon   # Build daemon only
make gui      # Build GUI only
```

### Clean Build
```bash
make clean    # Remove built binaries
```

### Install System-wide
```bash
sudo make install
```

This will:
- Copy binaries to `/usr/local/bin/`
- Install systemd service for the daemon (`/etc/systemd/system/dudes-control-center-fan-daemon.service`)
- Enable automatic startup on boot (if enabled)

### Uninstall
```bash
sudo make uninstall
```

## Configuration

### Profile Storage
Profiles are saved to: `~/.config/dudes-control-center/profiles.json`

### Log Files
**GUI Application:**
- Location: `~/.cache/dudes-control-center/logs/GUI.log`
- Rotation: 5 files, max 10MB each

**Daemon:**
- Location: `/var/log/dudes-control-center/DAEMON.log`
- Rotation: 5 files, max 10MB each
- Requires root access to read

### Unix Socket
- Path: `/run/dudes-control-center-fan.sock`
- Permissions: 666 (read/write for all users)

## Hardware Compatibility

These applications are designed for Clevo-based notebooks that use EC I/O port access at ports 0x62 and 0x66. The implementation is based on the open-source [fan-control](https://github.com/steiiin/fan-control) project.

**Tested hardware:**
- CPU: Intel Ultra 9
- GPU: NVIDIA RTX 5090 Mobile
- EC Registers: Standard Clevo layout

## Safety Features & Warnings

⚠️ **IMPORTANT:** These applications directly access hardware I/O ports to control the Embedded Controller. Use at your own risk.

### Built-in Safety Measures

1. **Watchdog Timer (Daemon)**
   - Monitors main loop responsiveness
   - Triggers after 10 seconds of unresponsiveness
   - Automatically sets fans to 100% in fail-safe mode

2. **Fail-Safe Mode**
   - Activates on critical errors
   - Sets fan speed to maximum (100%)
   - Prevents overheating in case of software failure

3. **Temperature Validation**
   - Invalid readings (< 10°C or > 110°C) trigger safety responses
   - Auto mode sets fans to 100% if sensors fail
   - Multiple fallback sources for temperature reading (EC, sysfs, nvidia-smi)

4. **Single Instance Protection**
   - GUI prevents multiple instances via PID file
   - Prevents conflicting fan control commands

### Safety Recommendations

1. Always test with moderate fan speeds first (20-40%)
2. Monitor temperatures closely when using manual mode
3. Keep the daemon running for continuous monitoring
4. Review logs regularly for warnings or errors
5. Have a backup cooling method available during initial testing

## Technical Details

### EC I/O Ports
- Status Command Port: `0x66` (EC_SC)
- Data Port: `0x62` (EC_DATA)

### EC Registers Used
| Register | Address | Description |
|----------|---------|-------------|
| CPU Temp | 0x07 | CPU temperature in °C |
| GPU Temp | 0xCD | GPU temperature in °C |
| Fan1 Duty | 0xCE | CPU fan duty cycle (0-255) |
| Fan2 Duty | 0xCF | GPU fan duty cycle (0-255) |
| Fan1 RPM HI | 0xD0 | High byte of CPU fan RPM |
| Fan1 RPM LO | 0xD1 | Low byte of CPU fan RPM |
| Fan2 RPM HI | 0xD2 | High byte of GPU fan RPM |
| Fan2 RPM LO | 0xD3 | Low byte of GPU fan RPM |

### Temperature Curve Calculation
The auto mode uses linear interpolation between defined temperature points. Default curve:
- ≤45°C: 20% fan speed
- 45-55°C: 20-30% (interpolated)
- 55-60°C: 30-40% (interpolated)
- 60-70°C: 40-50% (interpolated)
- 70-80°C: 50-70% (interpolated)
- 80-85°C: 70-100% (interpolated)
- ≥85°C: 100% fan speed

**Custom curves can be created and edited in the GUI Curve Editor tab.**

### RPM to Percentage Conversion
Based on user specification: ~1130 RPM = 10%
```c
percentage = min(100, rpm / 113.0)
```

### IPC Protocol
The daemon communicates via a simple text-based protocol over Unix socket:
- Commands: `STATUS`, `LIST_PROFILES`, `GET_PROFILE <index>`, `SET_MANUAL <percent>`, `ENABLE_AUTO <profile_index>`
- Responses: Text-based with structured data for parsing

## Troubleshooting

### "Cannot set I/O permission" error
Run with sudo: `sudo ./dudes-control-center-fan-cli`

### GUI says "Daemon is not running"
This is the most common issue. Follow these steps:

**Step 1: Start the daemon properly**
```bash
# In terminal 1, start the daemon in foreground mode
sudo ./dudes-control-center-fan-daemon --foreground
```
You should see output like:
```
dudes-control-center-fan-daemon starting...
Socket created at /run/dudes-control-center-fan.sock with permissions: 666
Socket owner: uid=0, gid=0
Server listening on /run/dudes-control-center-fan.sock
```

**Step 2: Verify the socket was created**
```bash
# In terminal 2, check if socket exists
ls -la /run/dudes-control-center-fan.sock
```
Expected output:
```
rw-rw-rw- 1 root root 0 Oct 25 22:38 /run/dudes-control-center-fan.sock
```
The permissions should be `666` (rw-rw-rw-) to allow non-root users to connect.

**Step 3: Launch the GUI**
```bash
# In terminal 3, run the GUI without sudo
./dudes-control-center-gui
```

### Socket permission issues
If you see "Permission denied" when connecting:

1. **Check socket permissions:**
   ```bash
   stat /run/dudes-control-center-fan.sock | grep -E "Access:|Uid|Gid"
   ```
   Should show `Access: (0666/rw-rw-rw-)` 

2. **Manually fix permissions (temporary):**
   ```bash
   sudo chmod 666 /run/dudes-control-center-fan.sock
   ```

3. **Restart the daemon:**
   ```bash
   sudo killall dudes-control-center-fan-daemon
   sudo ./dudes-control-center-fan-daemon --foreground
   ```

### Zero or invalid temperature readings
- Check if your notebook uses the same EC register layout
- Some models may have different GPU temp sensor locations
- The application falls back to multiple sources (EC, sysfs, nvidia-smi)
- Check daemon logs: `cat /var/log/dudes-control-center/DAEMON.log`

### Fan doesn't respond
- Your model may use different EC commands
- Some notebooks require specific initialization sequences
- Check if BIOS has fan control locked
- Verify hardware compatibility with Clevo EC layout

### GUI crashes or shows wrong state
- Check GUI logs: `cat ~/.cache/dudes-control-center/logs/GUI.log`
- Try switching tabs to refresh the UI state
- Restart both daemon and GUI if issues persist

### Watchdog triggered in logs
If you see "WATCHDOG TRIGGERED" in daemon logs:
- This indicates the main loop became unresponsive
- Fans were automatically set to 100% for safety
- Check system load and resource usage
- Review logs for preceding errors

## Testing the Connection

Use this quick test to verify daemon-GUI connectivity:

```bash
# Terminal 1: Start daemon
sudo ./dudes-control-center-fan-daemon --foreground &

# Terminal 2: Test socket connection
echo "STATUS" | socat - UNIX-CONNECT:/run/dudes-control-center-fan.sock
```

If `socat` is not installed:
```bash
sudo apt install socat
```

## Credits & Acknowledgments

This application was **completely coded by AI** using [Bionic AI Agent](https://lmstudio.ai/).

### Special Thanks To:
- **[LM Studio](https://lmstudio.ai/)** - For the Bionic AI Agent that created this entire application
- **[steiiin](https://github.com/steiiin/fan-control)** - Original creator of the fan-control source code that inspired this project
- **Linus Torvalds** - Founder of Linux [Blog](https://torvalds-family.blogspot.com/)
- **The Great Linux Community** - For continuous support and innovation
- **[Clevo](https://www.clevo.com.tw/)** - For producing and selling awesome laptops

### Author
**Dudemeister**
- Email: [wayne.dude@proton.me](mailto:wayne.dude@proton.me)
- GitHub: [dudewayne](https://github.com/dudewayne)

## License

This project uses code inspired by [fan-control](https://github.com/steiiin/fan-control). Please refer to the original project for its license terms.

## References

- Original fan-control: https://github.com/steiiin/fan-control
- Tuxedo Control Center (design reference): https://github.com/tuxedocomputers/tuxedo-control-center
- GTK+ Documentation: https://gtk.org/documentation/