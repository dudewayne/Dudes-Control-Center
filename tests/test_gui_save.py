# SPDX-License-Identifier: GPL-2.0-only
"""Runs test_gui_save (C) against a fake fan daemon and a test build of the CPU daemon.

Usage: python3 tests/test_gui_save.py <empty-work-dir> <test_gui_save binary> <cpud_test binary>

Both test binaries use relative socket paths ("fan.sock", "cpu.sock"), so
everything runs inside the work directory and nothing touches the real system.
"""
import json
import os
import socket
import subprocess
import sys
import threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fakesys import make_sysfs  # noqa: E402

base = os.path.abspath(sys.argv[1])
gui_test, cpud = os.path.abspath(sys.argv[2]), os.path.abspath(sys.argv[3])
os.chdir(base)

FAN_PROFILES = ["Default", "Quiet", "Loud"]


def fan_reply(line):
    """Minimal fan daemon: answers the commands the GUI uses when saving."""
    parts = line.split()
    if parts and parts[0] == "GET_PROFILE" and len(parts) > 1 and parts[1].isdigit():
        i = int(parts[1])
        if i < len(FAN_PROFILES):
            return (f"PROFILE_INFO name={FAN_PROFILES[i]} points=2 active={1 if i == 1 else 0}\n"
                    "CURVE_POINT temp=40 fan_pct=20\nCURVE_POINT temp=80 fan_pct=100\nPROFILE_END\n")
        return "ERROR Invalid profile index\n"
    return "OK\n"


def serve_fan(server):
    while True:
        try:
            conn, _ = server.accept()
        except OSError:
            return
        threading.Thread(target=handle, args=(conn,), daemon=True).start()


def handle(conn):
    buf = b""
    with conn:
        while True:
            data = conn.recv(4096)
            if not data:
                return
            buf += data
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                conn.sendall(fan_reply(line.decode()).encode())


# fake fan daemon
if os.path.exists("fan.sock"):
    os.unlink("fan.sock")
fan_server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
fan_server.bind("fan.sock")
fan_server.listen(5)
threading.Thread(target=serve_fan, args=(fan_server,), daemon=True).start()

# CPU daemon (test build) with its own config, separate from the GUI's HOME
make_sysfs("sys", 4)
os.makedirs("cfg", exist_ok=True)
os.makedirs("logs", exist_ok=True)
json.dump({"version": "1.0", "cpu_freq_profiles": {
    "active_profile_index": 0, "cpu_hard_limit_hz": 4000000000,
    "profiles": [{"name": "DaemonCpu", "governor": "powersave", "apply_all_cores": 1, "num_cores": 4,
                  "per_core_max_freq_hz": [0] * 4, "per_core_min_freq_hz": [0] * 4}]}},
    open("cfg/profiles.json", "w"))
cpu = subprocess.Popen([cpud, "--foreground"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

try:
    # wait for the CPU socket
    for _ in range(50):
        if os.path.exists("cpu.sock"):
            break
        threading.Event().wait(0.1)
    os.makedirs("home", exist_ok=True)
    result = subprocess.run([gui_test, os.path.join(base, "home")])
    rc = result.returncode
finally:
    cpu.terminate()
    cpu.wait(timeout=5)
    fan_server.close()

sys.exit(rc)
