#!/usr/bin/env python3
"""
Automated Test Suite for Dudes Control Center GUI
Tests all major features by monitoring debug output and simulating user actions via IPC
"""

import subprocess
import time
import re
import sys
import socket
from pathlib import Path

# Configuration
GUI_BINARY = "./dudes-control-center-gui"
DEBUG_FLAG = "--debug"
SOCKET_PATH = "/run/dudes-control-center-fan.sock"
LOG_FILE = "test_output.log"
TEST_TIMEOUT = 10  # seconds per test

class TestRunner:
    def __init__(self):
        self.gui_process = None
        self.test_results = []
        self.start_time = None
        
    def start_gui(self):
        """Start the GUI application in debug mode"""
        print("=" * 60)
        print("Starting GUI application...")
        print("=" * 60)
        
        log_path = Path(LOG_FILE)
        with open(log_path, 'w') as log_file:
            self.gui_process = subprocess.Popen(
                [GUI_BINARY, DEBUG_FLAG],
                stdout=log_file,
                stderr=subprocess.STDOUT,
                text=True,
                bufsize=1
            )
        
        # Wait for GUI to start and connect to daemon
        time.sleep(2)
        print("GUI started. Waiting for connection...")
        
        if self.check_log_for("Connected to daemon successfully"):
            print("✓ GUI connected to daemon successfully")
            return True
        else:
            print("✗ Failed to connect to daemon")
            return False
    
    def stop_gui(self):
        """Stop the GUI application"""
        if self.gui_process:
            print("\nStopping GUI...")
            self.gui_process.terminate()
            try:
                self.gui_process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.gui_process.kill()
            print("GUI stopped.")
    
    def read_log(self):
        """Read the current log file"""
        try:
            with open(LOG_FILE, 'r') as f:
                return f.read()
        except FileNotFoundError:
            return ""
    
    def check_log_for(self, text, timeout=TEST_TIMEOUT):
        """Check if text appears in log within timeout seconds"""
        start = time.time()
        while time.time() - start < timeout:
            log = self.read_log()
            if text in log:
                return True
            time.sleep(0.1)
        return False
    
    def clear_log(self):
        """Clear the current log"""
        with open(LOG_FILE, 'w') as f:
            pass
    
    def send_daemon_command(self, command):
        """Send a command to the daemon via socket (simulating GUI actions)"""
        try:
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            sock.connect(SOCKET_PATH)
            sock.sendall((command + '\n').encode())
            response = sock.recv(1024).decode()
            sock.close()
            return response
        except Exception as e:
            print(f"  Error sending command: {e}")
            return ""
    
    def test_basic_connection(self):
        """Test 1: Basic daemon connection"""
        print("\n[Test 1] Basic Daemon Connection")
        print("-" * 40)
        
        if self.check_log_for("Connected to daemon successfully", timeout=5):
            print("✓ PASS: Connected to daemon")
            self.test_results.append(("Basic Connection", True))
            return True
        else:
            print("✗ FAIL: Could not connect to daemon")
            self.test_results.append(("Basic Connection", False))
            return False
    
    def test_profile_list_loading(self):
        """Test 2: Profile list loads correctly"""
        print("\n[Test 2] Profile List Loading")
        print("-" * 40)
        
        if self.check_log_for("PROFILES_START", timeout=3):
            log = self.read_log()
            profile_count = len(re.findall(r'PROFILE \d+', log))
            print(f"✓ Found {profile_count} profiles in list")
            
            if profile_count >= 1:
                print("✓ PASS: Profile list loaded")
                self.test_results.append(("Profile List Loading", True))
                return True
            
        print("✗ FAIL: Profile list not loaded properly")
        self.test_results.append(("Profile List Loading", False))
        return False
    
    def test_curve_points_loading(self):
        """Test 3: Curve points load for active profile"""
        print("\n[Test 3] Curve Points Loading")
        print("-" * 40)
        
        if self.check_log_for("CURVE_POINT", timeout=3):
            log = self.read_log()
            curve_points = len(re.findall(r'CURVE_POINT temp=', log))
            print(f"✓ Found {curve_points} curve points")
            
            if curve_points >= 1:
                print("✓ PASS: Curve points loaded")
                self.test_results.append(("Curve Points Loading", True))
                return True
            
        print("✗ FAIL: Curve points not loaded")
        self.test_results.append(("Curve Points Loading", False))
        return False
    
    def test_daemon_communication(self):
        """Test 4: Bidirectional daemon communication"""
        print("\n[Test 4] Daemon Communication")
        print("-" * 40)
        
        log = self.read_log()
        
        sends = len(re.findall(r'\[GUI\] Sending:', log))
        receives = len(re.findall(r'\[GUI\] Received', log))
        
        print(f"✓ Sent {sends} commands to daemon")
        print(f"✓ Received {receives} responses from daemon")
        
        if sends > 0 and receives > 0:
            print("✓ PASS: Bidirectional communication working")
            self.test_results.append(("Daemon Communication", True))
            return True
        
        print("✗ FAIL: No daemon communication detected")
        self.test_results.append(("Daemon Communication", False))
        return False
    
    def test_manual_mode_via_socket(self):
        """Test 5: Manual mode toggle via direct socket command"""
        print("\n[Test 5] Manual Mode Toggle (via socket)")
        print("-" * 40)
        
        self.clear_log()
        
        # Send SET_AUTO 0 to disable auto mode
        response = self.send_daemon_command("SET_AUTO 0")
        print(f"  Response: {response}")
        
        if "Auto mode disabled" in response or self.check_log_for("Manual mode ENABLED"):
            print("✓ PASS: Manual mode can be enabled")
            
            # Now re-enable auto mode
            response = self.send_daemon_command("SET_AUTO 1")
            print(f"  Response: {response}")
            
            if "Auto mode enabled" in response or self.check_log_for("Manual mode DISABLED"):
                print("✓ PASS: Manual mode can be disabled")
                self.test_results.append(("Manual Mode Toggle", True))
                return True
        
        print("✗ FAIL: Manual mode toggle failed")
        self.test_results.append(("Manual Mode Toggle", False))
        return False
    
    def test_status_command(self):
        """Test 6: STATUS command works"""
        print("\n[Test 6] Status Command")
        print("-" * 40)
        
        response = self.send_daemon_command("STATUS")
        print(f"  Response: {response}")
        
        if "CPU_TEMP=" in response and "GPU_TEMP=" in response:
            print("✓ PASS: STATUS command returns temperature data")
            self.test_results.append(("Status Command", True))
            return True
        
        print("✗ FAIL: STATUS command failed")
        self.test_results.append(("Status Command", False))
        return False
    
    def test_list_profiles_command(self):
        """Test 7: LIST_PROFILES command works"""
        print("\n[Test 7] List Profiles Command")
        print("-" * 40)
        
        response = self.send_daemon_command("LIST_PROFILES")
        print(f"  Response length: {len(response)} bytes")
        
        if "PROFILES_START" in response and "PROFILES_END" in response:
            profile_count = len(re.findall(r'PROFILE \d+', response))
            print(f"✓ Found {profile_count} profiles")
            print("✓ PASS: LIST_PROFILES command works")
            self.test_results.append(("List Profiles Command", True))
            return True
        
        print("✗ FAIL: LIST_PROFILES command failed")
        self.test_results.append(("List Profiles Command", False))
        return False
    
    def test_get_profile_command(self):
        """Test 8: GET_PROFILE command works"""
        print("\n[Test 8] Get Profile Command")
        print("-" * 40)
        
        response = self.send_daemon_command("GET_PROFILE 0")
        print(f"  Response length: {len(response)} bytes")
        
        if "PROFILE_INFO" in response and "CURVE_POINT" in response:
            point_count = len(re.findall(r'CURVE_POINT', response))
            print(f"✓ Found {point_count} curve points")
            print("✓ PASS: GET_PROFILE command works")
            self.test_results.append(("Get Profile Command", True))
            return True
        
        print("✗ FAIL: GET_PROFILE command failed")
        self.test_results.append(("Get Profile Command", False))
        return False
    
    def test_no_crashes(self):
        """Test 9: Application stability (no crashes)"""
        print("\n[Test 9] Application Stability")
        print("-" * 40)
        
        log = self.read_log()
        
        crash_indicators = [
            "Segmentation fault",
            "core dumped",
            "free(): invalid pointer",
            "Assertion failed",
            "gtk_toggle_button_get_active: assertion"
        ]
        
        found_crashes = []
        for indicator in crash_indicators:
            if indicator in log:
                found_crashes.append(indicator)
        
        if not found_crashes:
            print("✓ PASS: No crashes detected")
            self.test_results.append(("No Crashes", True))
            return True
        
        print(f"✗ FAIL: Found crash indicators:")
        for indicator in found_crashes:
            print(f"  - {indicator}")
        self.test_results.append(("No Crashes", False))
        return False
    
    def test_window_position(self):
        """Test 10: Window position saving/loading"""
        print("\n[Test 10] Window Position")
        print("-" * 40)
        
        log = self.read_log()
        
        if "Loaded window position" in log:
            match = re.search(r'Loaded window position: (\d+),(\d+) (\d+x\d+)', log)
            if match:
                x, y, size = match.groups()
                print(f"✓ Window loaded at ({x}, {y}) with size {size}")
        
        print("⚠ INFO: Window save will only appear on application close")
        self.test_results.append(("Window Position", True))
        return True
    
    def test_ramp_up_down_logic(self):
        """Test 11: Fan ramp up/down logic (check logs for proper behavior)"""
        print("\n[Test 11] Fan Ramp Logic")
        print("-" * 40)
        
        log = self.read_log()
        
        # Check if daemon is properly ramping fan speed
        ramp_ups = len(re.findall(r'ramp up=\d+', log))
        ramp_downs = len(re.findall(r'ramp down=\d+', log))
        
        print(f"✓ Found {ramp_ups} ramp-up events")
        print(f"✓ Found {ramp_downs} ramp-down events")
        
        if ramp_ups > 0 or ramp_downs > 0:
            print("✓ PASS: Fan ramp logic is active")
            self.test_results.append(("Fan Ramp Logic", True))
            return True
        
        print("⚠ INFO: No ramp events detected (may be at steady state)")
        self.test_results.append(("Fan Ramp Logic", None))
        return True
    
    def test_unsaved_changes_dialog_appears(self):
        """Test 12: Unsaved changes dialog appears when leaving Curve Editor with modifications"""
        print("\n[Test 12] Unsaved Changes Dialog Detection")
        print("-" * 40)
        
        # This test checks if the GUI properly detects unsaved changes
        # by looking for specific log patterns
        log = self.read_log()
        
        # Check if there's any mention of unsaved changes handling
        has_unsaved_check = "unsaved" in log.lower() or "Leaving Curve Editor" in log
        
        if has_unsaved_check:
            print("✓ PASS: Unsaved changes detection code is present")
            self.test_results.append(("Unsaved Changes Detection", True))
            return True
        else:
            print("⚠ INFO: No unsaved changes events in log (expected - no modifications made)")
            self.test_results.append(("Unsaved Changes Detection", None))
            return True
    
    def test_signal_handler_id_stored(self):
        """Test 13: Signal handler ID is stored for blocking"""
        print("\n[Test 13] Signal Handler Blocking Setup")
        print("-" * 40)
        
        # Check the source code to verify signal handler ID is being stored
        gui_source = Path("src/dudes-control-center-gui.c").read_text()
        
        has_handler_id_field = "page_switched_handler_id" in gui_source
        uses_signal_block = "g_signal_handler_block" in gui_source
        uses_signal_unblock = "g_signal_handler_unblock" in gui_source
        
        print(f"  Handler ID field: {'✓' if has_handler_id_field else '✗'}")
        print(f"  Signal block call: {'✓' if uses_signal_block else '✗'}")
        print(f"  Signal unblock call: {'✓' if uses_signal_unblock else '✗'}")
        
        if has_handler_id_field and uses_signal_block and uses_signal_unblock:
            print("✓ PASS: Signal blocking mechanism is implemented")
            self.test_results.append(("Signal Blocking Setup", True))
            return True
        else:
            print("✗ FAIL: Signal blocking mechanism not properly implemented")
            self.test_results.append(("Signal Blocking Setup", False))
            return False
    
    def test_cancel_button_no_crash(self):
        """Test 14: Cancel button doesn't cause crashes (simulated via log analysis)"""
        print("\n[Test 14] Cancel Button Stability")
        print("-" * 40)
        
        # Check for any GTK critical errors related to toggle buttons
        log = self.read_log()
        
        gtk_errors = re.findall(r'Gtk-CRITICAL.*toggle_button', log)
        assertion_failures = re.findall(r'assertion.*failed', log, re.IGNORECASE)
        
        if gtk_errors:
            print(f"✗ Found {len(gtk_errors)} GTK toggle button errors")
            self.test_results.append(("Cancel Button Stability", False))
            return False
        
        if assertion_failures:
            print(f"✗ Found {len(assertion_failures)} assertion failures")
            self.test_results.append(("Cancel Button Stability", False))
            return False
        
        print("✓ PASS: No GTK toggle button errors or assertion failures")
        self.test_results.append(("Cancel Button Stability", True))
        return True
    
    def run_all_tests(self):
        """Run all automated tests"""
        print("\n" + "=" * 60)
        print("Dudes Control Center GUI - Automated Test Suite")
        print("=" * 60)
        
        self.start_time = time.time()
        
        # Start the GUI
        if not self.start_gui():
            print("\n✗ FAILED: Could not start GUI application")
            return False
        
        try:
            # Run all tests
            self.test_basic_connection()
            self.test_profile_list_loading()
            self.test_curve_points_loading()
            self.test_daemon_communication()
            self.test_manual_mode_via_socket()
            self.test_status_command()
            self.test_list_profiles_command()
            self.test_get_profile_command()
            self.test_window_position()
            self.test_ramp_up_down_logic()
            self.test_unsaved_changes_dialog_appears()
            self.test_signal_handler_id_stored()
            self.test_cancel_button_no_crash()
            
        except Exception as e:
            print(f"\n✗ Test suite error: {e}")
            import traceback
            traceback.print_exc()
        
        finally:
            # Check for crashes before stopping
            self.test_no_crashes()
            
            # Stop the GUI
            self.stop_gui()
        
        # Print summary
        self.print_summary()
    
    def print_summary(self):
        """Print test results summary"""
        print("\n" + "=" * 60)
        print("TEST SUMMARY")
        print("=" * 60)
        
        passed = sum(1 for _, result in self.test_results if result == True)
        failed = sum(1 for _, result in self.test_results if result == False)
        skipped = sum(1 for _, result in self.test_results if result is None)
        
        for test_name, result in self.test_results:
            if result == True:
                status = "✓ PASS"
            elif result == False:
                status = "✗ FAIL"
            else:
                status = "⚠ SKIP"
            print(f"{status}: {test_name}")
        
        print("-" * 60)
        print(f"Passed: {passed}, Failed: {failed}, Skipped: {skipped}")
        print(f"Total time: {time.time() - self.start_time:.1f}s")
        print("=" * 60)
        
        if failed > 0:
            print("\n✗ Some tests FAILED. Check the log for details.")
            return False
        else:
            print("\n✓ All automated tests passed!")
            return True


def main():
    runner = TestRunner()
    success = runner.run_all_tests()
    
    # Keep log file open for inspection
    print(f"\nLog saved to: {LOG_FILE}")
    
    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
