"""Shared helpers for the Python tests: a fake cpufreq sysfs tree and a
client for the daemons' newline-framed socket protocol."""
import os
import socket
import time


def wr(path, val):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(str(val))


def make_sysfs(sysfs, ncpu=24, max_khz=5300000, min_khz=800000, cur_khz=5100000):
    """Create <sysfs>/cpuN/{topology,cpufreq/*} like /sys/devices/system/cpu."""
    for c in range(ncpu):
        os.makedirs(f"{sysfs}/cpu{c}/topology", exist_ok=True)
        d = f"{sysfs}/cpu{c}/cpufreq"
        wr(f"{d}/cpuinfo_max_freq", max_khz)
        wr(f"{d}/cpuinfo_min_freq", min_khz)
        wr(f"{d}/scaling_max_freq", max_khz)
        wr(f"{d}/scaling_min_freq", min_khz)
        wr(f"{d}/scaling_cur_freq", cur_khz)
        wr(f"{d}/scaling_governor", "powersave")
        wr(f"{d}/energy_performance_preference", "balance_performance")
    wr(f"{sysfs}/cpufreq/policy0/scaling_available_governors", "performance powersave\n")
    wr(f"{sysfs}/intel_pstate/min_perf_pct", 20)


def read_khz(sysfs, cpu, name):
    with open(f"{sysfs}/cpu{cpu}/cpufreq/{name}") as f:
        return int(f.read().strip())


def connect(path, timeout=2.0):
    """Connect to a socket path relative to the current directory
    (absolute test paths can exceed the 108-byte AF_UNIX limit)."""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    deadline = time.time() + timeout
    while True:
        try:
            s.connect(path)
            return s
        except OSError:
            if time.time() > deadline:
                raise
            time.sleep(0.1)


def ask(s, cmd, newline=True, wait=0.4):
    s.sendall((cmd + ("\n" if newline else "")).encode())
    time.sleep(wait)
    data = b""
    s.settimeout(0.5)
    try:
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            data += chunk
    except socket.timeout:
        pass
    return data.decode()


class Checker:
    def __init__(self):
        self.fails = 0

    def __call__(self, cond, msg):
        print(("PASS  " if cond else "FAIL  ") + msg, flush=True)
        if not cond:
            self.fails += 1

    def finish(self):
        print(f"\n{self.fails} failure(s)")
        return 1 if self.fails else 0
