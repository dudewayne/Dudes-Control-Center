"""End-to-end test of the CPU daemon against a fake sysfs (no root, no hardware).

Usage: python3 tests/test_cpu_daemon.py <empty-work-dir> <cpud_test binary>

The test binary is the real daemon built by "make test" with relative paths:
socket "cpu.sock", sysfs "sys", config "cfg", logs "logs" (all inside the
work directory). Covers the v1.16.4 fixes: 64-bit frequencies, hard limit as a
ceiling, per-core min/max enforcement, corrupt values, several clients,
long batch commands, signals.
"""
import json
import os
import signal
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fakesys import Checker, ask, connect, make_sysfs, read_khz, wr  # noqa: E402

base, daemon = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
os.chdir(base)
NCPU = 24
check = Checker()

make_sysfs("sys", NCPU)
wr("sys/cpu3/cpufreq/scaling_min_freq", 3600000)  # stale min, profile says 0.8 GHz
wr("sys/cpu4/cpufreq/scaling_min_freq", 1500000)  # profile sets no min -> leave alone
os.makedirs("cfg", exist_ok=True)
os.makedirs("logs", exist_ok=True)

profile = {
    # schedutil is not offered by the fake driver (like intel_pstate in active mode)
    "name": "Test", "governor": "schedutil", "apply_all_cores": 0, "num_cores": NCPU,
    # core 0 below the limit; core 10 = 3 Hz, a value truncated by the old 256-byte GUI limit
    "per_core_max_freq_hz": [3000000000] + [0] * 9 + [3] + [0] * (NCPU - 11),
    "per_core_min_freq_hz": [0, 0, 0, 800000000] + [0] * (NCPU - 4),
}
json.dump({"version": "1.0",
           "keyboard_profiles": {"active_profile_index": 0, "profiles": [{"name": "kb"}]},
           "cpu_freq_profiles": {"active_profile_index": 0, "cpu_hard_limit_hz": 5000000000,
                                 "profiles": [profile]}},
          open("cfg/profiles.json", "w"))

proc = subprocess.Popen([daemon, "--foreground"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    a = connect("cpu.sock", timeout=5)
    time.sleep(1.2)   # first enforcement pass

    # 64-bit frequencies
    r = ask(a, "GET_HARD_LIMIT")
    check("HARD_LIMIT=5000000000" in r, "hard limit above 4.29 GHz round-trips")
    r = ask(a, "GET_CPU_INFO|1")
    check("CURRENT_FREQ=5.10 GHz" in r, "5.1 GHz current frequency read without 32-bit overflow")

    # hard limit is a ceiling; per-core values kept
    check(read_khz("sys", 0, "scaling_max_freq") == 3000000, "core 0 keeps its profile max (3.0 GHz)")
    check(read_khz("sys", 5, "scaling_max_freq") == 5000000, "core 5 capped at the 5.0 GHz hard limit")
    check(read_khz("sys", 10, "scaling_max_freq") == 5000000, "corrupt 3 Hz value ignored (hard limit, not 800 MHz)")
    check(read_khz("sys", 3, "scaling_min_freq") == 800000, "stale higher min lowered to the profile's min")
    check(read_khz("sys", 4, "scaling_min_freq") == 1500000, "min left alone where the profile sets none")

    # values changed by someone else are re-applied within 5 s
    wr("sys/cpu2/cpufreq/scaling_max_freq", 5300000)
    wr("sys/cpu7/cpufreq/scaling_max_freq", 2000000)
    time.sleep(5.5)
    check(read_khz("sys", 2, "scaling_max_freq") == 5000000, "core raised above the limit is clamped back")
    check(read_khz("sys", 7, "scaling_max_freq") == 5000000, "core lowered externally is restored")
    check(read_khz("sys", 0, "scaling_max_freq") == 3000000, "profile value still not raised to the limit")

    # governors: only what the driver offers (v1.17.1)
    r = ask(a, "LIST_GOVERNORS")
    check("Performance" in r and "Power Save" in r and "Balanced" not in r,
          "governor policies only for available governors (no 'Balanced'/schedutil)")
    r = ask(a, "APPLY_CUSTOM_PROFILE|0")
    gov = open("sys/cpu0/cpufreq/scaling_governor").read().strip()
    check(gov == "powersave", f"profile asking for schedutil gets the closest available governor (got {gov})")
    r = ask(a, "APPLY_GOVERNOR|Performance")
    gov = open("sys/cpu0/cpufreq/scaling_governor").read().strip()
    check("OK" in r and gov == "performance", "available governor applied as-is")

    # several clients at once
    b = connect("cpu.sock")
    check("PONG" in ask(b, "PING") and "PONG" in ask(a, "PING"), "two clients are served at the same time")
    c = connect("cpu.sock")
    check("PONG" in ask(c, "PING", newline=False), "command without newline answered after idle timeout")

    # long batch command with 64-bit values
    pairs = ",".join(f"{800000000 + i},{5000000000 - i}" for i in range(NCPU))
    cmd = f"SAVE_CUSTOM_PROFILE|0|performance|0|{pairs}"
    check(len(cmd) > 256, f"batch command longer than the old 256-byte limit ({len(cmd)} bytes)")
    check(f"({NCPU} cores)" in ask(a, cmd), f"all {NCPU} cores stored")
    r = ask(a, "GET_CUSTOM_PROFILE_INFO|0")
    check(f"CORE_{NCPU - 1}_MAX:{5000000000 - (NCPU - 1)}" in r, "last core's 64-bit value read back exactly")

    pairs_sched = ",".join(f"800000000,{4000000000}" for _ in range(NCPU))
    ask(a, f"SAVE_CUSTOM_PROFILE|0|schedutil|0|{pairs_sched}")
    check("GOVERNOR:powersave" in ask(a, "GET_CUSTOM_PROFILE_INFO|0"),
          "saving with an unavailable governor stores an available one")

    # saving keeps the other sections
    ask(a, "SET_HARD_LIMIT|4800000000")
    data = json.load(open("cfg/profiles.json"))
    check(data["cpu_freq_profiles"]["cpu_hard_limit_hz"] == 4800000000, "new hard limit persisted")
    check("keyboard_profiles" in data, "keyboard section preserved by the daemon's save")

    # signals
    proc.send_signal(signal.SIGHUP)
    time.sleep(1.5)
    check("PONG" in ask(a, "PING"), "daemon still answers after SIGHUP")
    for s in (a, b, c):
        s.close()
    proc.send_signal(signal.SIGTERM)
    proc.wait(timeout=5)
    check(proc.returncode == 0, "SIGTERM -> clean exit (no sanitizer errors)")
finally:
    if proc.poll() is None:
        proc.kill()

log = open("logs/CPU_DAEMON.log").read()
check("SIGHUP: reloading configuration" in log, "SIGHUP reload runs in the main loop")
check("core 10: invalid max frequency 3 Hz" in log, "corrupt per-core value reported in the log")
check("uses governor 'schedutil', which this system does not offer" in log, "unavailable governor reported at startup")
sys.exit(check.finish())
