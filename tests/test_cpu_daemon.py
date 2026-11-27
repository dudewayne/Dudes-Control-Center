# SPDX-License-Identifier: GPL-2.0-only
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
# fake RAPL (v1.37.0): MSR and MMIO limits like the X58xWNx firmware sets them
wr("powercap/intel-rapl:0/energy_uj", 1000000)
wr("powercap/intel-rapl:0/max_energy_range_uj", 262143328850)
wr("powercap/intel-rapl:0/enabled", 1)
wr("powercap/intel-rapl:0/constraint_0_power_limit_uw", 200000000)
wr("powercap/intel-rapl-mmio:0/enabled", 1)
wr("powercap/intel-rapl-mmio:0/constraint_0_power_limit_uw", 46000000)
# firmware power profile (v1.43.0) and an AMD CPU sensor (v1.42.0)
wr("acpi/platform_profile_choices", "low-power balanced performance")
wr("acpi/platform_profile", "balanced")
# battery with a charge limit (1.46.0); a limit of 75 % is stored from before
wr("ps/AC/type", "Mains\n")
wr("ps/BAT0/type", "Battery\n")
wr("ps/BAT0/charge_control_end_threshold", "100\n")
wr("ps/BAT0/charge_control_start_threshold", "90\n")
wr("state/battery-limit", "75\n")
wr("hwmon/hwmon3/name", "k10temp")
wr("hwmon/hwmon3/temp1_label", "Tctl")
wr("hwmon/hwmon3/temp1_input", 71000)
wr("hwmon/hwmon3/temp2_label", "Tdie")
wr("hwmon/hwmon3/temp2_input", 61000)
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

    # no forced settings at startup (v1.17.1)
    epp0 = open("sys/cpu0/cpufreq/energy_performance_preference").read().strip()
    check(epp0 == "default", f"startup no longer forces EPP 'performance' (EPP is '{epp0}')")
    pct = open("sys/intel_pstate/min_perf_pct").read().strip()
    check(pct == "0", f"min_perf_pct=100 left by older versions reset to the driver minimum (got {pct})")

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

    # power modes = governor + EPP; the fake driver offers only performance/powersave (v1.17.1)
    def gov_epp(cpu=0):
        g = open(f"sys/cpu{cpu}/cpufreq/scaling_governor").read().strip()
        e = open(f"sys/cpu{cpu}/cpufreq/energy_performance_preference").read().strip()
        return g, e

    r = ask(a, "LIST_GOVERNORS")
    check("Performance (gov=performance, epp=-" in r, "Performance policy: performance governor, EPP untouched")
    check("Balanced (gov=powersave, epp=balance_performance" in r,
          "Balanced policy without schedutil: powersave + EPP balance_performance")
    check("Power Save (gov=powersave, epp=power" in r, "Power Save policy: powersave + EPP power")
    check("schedutil" not in r, "no policy uses the unavailable schedutil")

    ask(a, "APPLY_CUSTOM_PROFILE|0")
    check(gov_epp(0) == ("powersave", "balance_performance") and gov_epp(23) == ("powersave", "balance_performance"),
          f"old profile with 'schedutil' applies as Balanced (got {gov_epp(0)})")
    ask(a, "APPLY_GOVERNOR|Power Save")
    check(gov_epp(5) == ("powersave", "power"), f"Power Save applies powersave + power (got {gov_epp(5)})")
    r = ask(a, "APPLY_GOVERNOR|Performance")
    check("OK" in r and gov_epp(5) == ("performance", "power"),
          "Performance sets the governor and leaves EPP to it")

    # STATUS shows the mode the CPU really runs (v1.20.3: was always "Balanced")
    st = ask(a, "STATUS")
    check("ACTIVE_GOVERNOR=Performance\n" in st and "GOVERNOR=performance\n" in st,
          "STATUS: Performance after switching to Performance")
    ask(a, "APPLY_GOVERNOR|Power Save")
    st = ask(a, "STATUS")
    check("ACTIVE_GOVERNOR=Power Save\n" in st and "EPP=power\n" in st, "STATUS: Power Save (powersave + power)")
    ask(a, "APPLY_GOVERNOR|Balanced")
    st = ask(a, "STATUS")
    check("ACTIVE_GOVERNOR=Balanced\n" in st and "EPP=balance_performance\n" in st, "STATUS: Balanced")
    # SET_POWER_MODE: governor + EPP only, the profile's core limits stay (v1.20.4)
    wr("sys/cpu0/cpufreq/scaling_max_freq", 3000000)
    r = ask(a, "SET_POWER_MODE|Power Save")
    check(r.startswith("OK") and gov_epp(0) == ("powersave", "power") and read_khz("sys", 0, "scaling_max_freq") == 3000000,
          "SET_POWER_MODE switches governor + EPP and keeps the core limits")
    check(ask(a, "SET_POWER_MODE|Turbo").startswith("ERROR"), "unknown power mode rejected")
    ask(a, "APPLY_GOVERNOR|Performance")

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
    r = ask(a, "GET_CUSTOM_PROFILE_INFO|0")
    check("GOVERNOR:powersave" in r and "EPP:balance_performance" in r,
          "saving 'schedutil' stores the Balanced equivalent (powersave + balance_performance)")
    ask(a, f"SAVE_CUSTOM_PROFILE|0|powersave:power|0|{pairs_sched}")
    r = ask(a, "GET_CUSTOM_PROFILE_INFO|0")
    check("GOVERNOR:powersave" in r and "EPP:power" in r, "explicit 'powersave:power' stored as given")
    ask(a, f"SAVE_CUSTOM_PROFILE|0|performance:power|0|{pairs_sched}")
    r = ask(a, "GET_CUSTOM_PROFILE_INFO|0")
    check("GOVERNOR:performance" in r and "EPP:\n" in r, "no EPP stored for the performance governor")
    check('"epp"' in ask(a, "LIST_CUSTOM_PROFILES_JSON"), "EPP included in the profile JSON")

    # firmware power profile in a CPU profile (v1.43.0)
    st = ask(a, "STATUS")
    check("PLATFORM_PROFILE=balanced\n" in st and "PLATFORM_PROFILE_CHOICES=low-power balanced performance\n" in st,
          "STATUS: current firmware power profile and the choices")
    check("CPU_TEMP=61\n" in st, "CPU temperature from k10temp's Tdie (v1.42.0)")
    r = ask(a, "SET_CUSTOM_PROFILE_PLATFORM|0|turbo")
    check(r.startswith("ERROR") and "not offered" in r, "a choice the laptop does not offer is refused")
    r = ask(a, "SET_CUSTOM_PROFILE_PLATFORM|0|performance")
    check(r.startswith("OK") and "PLATFORM_PROFILE:performance" in ask(a, "GET_CUSTOM_PROFILE_INFO|0"),
          "profile stores the firmware power profile")
    data = json.load(open("cfg/profiles.json"))
    check(data["cpu_freq_profiles"]["profiles"][0].get("platform_profile") == "performance",
          "firmware power profile saved in profiles.json")
    wr("acpi/platform_profile", "low-power")
    ask(a, "APPLY_CUSTOM_PROFILE|0")
    check(open("acpi/platform_profile").read().strip() == "performance", "applying the profile sets it")
    ask(a, "SET_CUSTOM_PROFILE_PLATFORM|0|-")
    wr("acpi/platform_profile", "low-power")
    ask(a, "APPLY_CUSTOM_PROFILE|0")
    check(open("acpi/platform_profile").read().strip() == "low-power" and
          "PLATFORM_PROFILE:\n" in ask(a, "GET_CUSTOM_PROFILE_INFO|0"), "'unchanged' leaves it alone")

    # saving keeps the other sections; the hard limit no longer touches EPP
    wr("sys/cpu1/cpufreq/energy_performance_preference", "balance_power")
    ask(a, "SET_HARD_LIMIT|4800000000")
    check(gov_epp(1)[1] == "balance_power", "SET_HARD_LIMIT no longer forces EPP 'performance'")
    data = json.load(open("cfg/profiles.json"))
    check(data["cpu_freq_profiles"]["cpu_hard_limit_hz"] == 4800000000, "new hard limit persisted")
    check("keyboard_profiles" in data, "keyboard section preserved by the daemon's save")

    # log level (v1.34.3): queries only at DEBUG, changes at INFO
    check(ask(a, "GET_LOG_LEVEL").startswith("LOG_LEVEL=INFO"), "GET_LOG_LEVEL: INFO by default")
    ask(a, "GET_HARD_LIMIT")
    check(ask(a, "SET_LOG_LEVEL|DEBUG").startswith("OK Log level set to DEBUG"), "SET_LOG_LEVEL|DEBUG accepted")
    ask(a, "GET_CPU_INFO")
    check(ask(a, "SET_LOG_LEVEL|info").startswith("OK Log level set to INFO"), "SET_LOG_LEVEL accepts lower case")
    check(ask(a, "SET_LOG_LEVEL|LOUD").startswith("ERROR"), "SET_LOG_LEVEL rejects unknown levels")
    ask(a, "GET_AVAILABLE_GOVERNORS")
    time.sleep(0.3)
    lv = open("logs/CPU_DAEMON.log").read()
    check("Received command: STATUS" not in lv and "Received command: GET_HARD_LIMIT" not in lv,
          "queries (STATUS, GET_*) not logged at INFO")
    check("Received command: GET_CPU_INFO" in lv and "Received command: GET_AVAILABLE_GOVERNORS" not in lv,
          "queries logged while the level is DEBUG only")
    check("Received command: SET_HARD_LIMIT" in lv, "commands that change something still logged at INFO")

    # CPU package power from RAPL (v1.37.0): energy difference / time
    wr("powercap/intel-rapl:0/energy_uj", 5000000000)
    ask(a, "STATUS")
    t0 = time.time()
    time.sleep(1.0)
    wr("powercap/intel-rapl:0/energy_uj", 5000000000 + 20000000)   # +20 J
    st = ask(a, "STATUS")
    dt = time.time() - t0
    w = float(st.split("CPU_POWER_W=")[1].split("\n")[0])
    check(abs(w - 20.0 / dt) < 3.0, f"CPU_POWER_W from the energy counter ({w:.1f} W, expected about {20.0 / dt:.1f})")
    check("CPU_POWER_LIMIT_W=46\n" in st, "sustained limit: the lower of MSR (200 W) and MMIO (46 W)")
    wr("powercap/intel-rapl:0/max_energy_range_uj", 6000000000)
    wr("powercap/intel-rapl:0/energy_uj", 1000000)                  # wrapped
    time.sleep(1.0)
    w2 = float(ask(a, "STATUS").split("CPU_POWER_W=")[1].split("\n")[0])
    check(0 < w2 < 1500, f"counter wrap-around handled ({w2:.0f} W)")

    # battery charge limit (1.46.0)
    def bl(f="end"):
        return open(f"ps/BAT0/charge_control_{f}_threshold").read().strip()
    check(bl() == "75" and bl("start") == "65", "stored charge limit set at start, start threshold lowered below it")
    st = ask(a, "STATUS")
    check("BATTERY_LIMIT=75\nBATTERY_LIMIT_SAVED=75\nBATTERY_LIMIT_DEVICE=BAT0\n" in st, "STATUS reports the charge limit")
    r = ask(a, "SET_BATTERY_LIMIT|80")
    check(r.startswith("OK Charge limit 80%") and bl() == "80" and open("state/battery-limit").read().strip() == "80",
          "SET_BATTERY_LIMIT sets and stores it")
    check(all(ask(a, f"SET_BATTERY_LIMIT|{v}").startswith("ERROR") for v in ("49", "101", "x", "")) and bl() == "80",
          "out-of-range or invalid limits are refused")
    wr("ps/BAT0/charge_control_end_threshold", "100\n")      # firmware reset (standby)
    time.sleep(3.2)
    check(bl() == "80", "a limit reset by the firmware is set again")
    r = ask(a, "SET_BATTERY_LIMIT|100")
    check(r.startswith("OK Charge limit off") and bl() == "100" and not os.path.exists("state/battery-limit"),
          "100 = off: charges to 100 %, nothing stored")
    os.rename("ps/BAT0/charge_control_end_threshold", "ps/BAT0/gone")
    check(ask(a, "SET_BATTERY_LIMIT|80").startswith("ERROR This laptop") and "BATTERY_LIMIT=-1\n" in ask(a, "STATUS"),
          "a battery without a charge limit: refused, STATUS -1")
    os.rename("ps/BAT0/gone", "ps/BAT0/charge_control_end_threshold")

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
startup = log.split("SIGHUP: reloading configuration")[0]
check(startup.count("custom profiles from") == 1,
      f"profiles loaded once at startup (v1.18.2; loaded {startup.count('custom profiles from')}x)")
check(startup.count("which this system does not offer") == 1, "unavailable-governor warning logged once at startup")
check("Could not load saved profiles" not in log, "no false 'could not load saved profiles' warning")
check(log.split("SIGHUP: reloading configuration")[1].count("custom profiles from") == 1, "SIGHUP reloads the profiles")
check("core 10: invalid max frequency 3 Hz" in log, "corrupt per-core value reported in the log")
check("uses governor 'schedutil', which this system does not offer" in log, "unavailable governor reported at startup")
check("BAT0: charge limit set to 75% (was 100%)" in log and "BAT0: charge limit back to 80% (the firmware had set 100%)" in log,
      "charge limit logged at start and after a firmware reset")
sys.exit(check.finish())
