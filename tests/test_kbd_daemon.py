# SPDX-License-Identifier: GPL-2.0-only
"""End-to-end test of the keyboard backlight daemon against a fake
/sys/class/leds (no root, no hardware).

Usage: python3 tests/test_kbd_daemon.py <empty-work-dir> <kbdd_test binary>

The test binary is the real daemon built by "make test" with relative paths:
socket "kbd.sock", LEDs "leds", profiles "cfg/profiles.json", logs "logs".
Covers per-key lighting (v1.18.0): SET_KEYS_COLOR, long LED lists, zones per
color, saving and restoring profiles, TURN_ON keeping the colors; several
clients and newline framing (v1.18.5).
"""
import json
import os
import signal
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fakesys import Checker, ask, connect, wr  # noqa: E402

base, daemon = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
os.chdir(base)
NLEDS = 120
check = Checker()

# fake LED class devices like ite_829x: rgb:kbd_backlight, rgb:kbd_backlight_1..119
names = ["rgb:kbd_backlight"] + [f"rgb:kbd_backlight_{i}" for i in range(1, NLEDS)]
for n in names:
    wr(f"leds/{n}/multi_intensity", "0 0 0\n")
    wr(f"leds/{n}/max_brightness", "10\n")      # ite_829x: hardware brightness 0-10
    wr(f"leds/{n}/brightness", "0\n")
wr("leds/input20::capslock/brightness", "0")   # not a keyboard RGB LED
os.makedirs("logs", exist_ok=True)

# Data saved by versions before 1.18.7 numbered the LEDs in directory order
# (what readdir returns). The daemon converts it once to name numbering.
old_order = [n for n in os.listdir("leds") if "kbd" in n]   # same filter and order as before


def num(name):
    return 0 if name.endswith("kbd_backlight") else int(name.rsplit("_", 1)[1])


old_of = {num(n): i for i, n in enumerate(old_order)}       # LED number -> old index
json.dump({"version": "1.0",
           "keyboard_profiles": {"active_profile_index": 1, "profiles": [
               {"name": "Empty", "zones": []},
               {"name": "Start", "zones": [{"name": "z", "r": 10, "g": 20, "b": 30,
                                            "keys": [old_of[0], old_of[1]]}]}]},
           "cpu_freq_profiles": {"keep": 1}},
          open(os.makedirs("cfg", exist_ok=True) or "cfg/profiles.json", "w"))
with open("cfg/keyboard_led_mapping.csv", "w") as f:        # wizard mapping, old numbering
    f.write("led_id;position;led_index\n")
    f.write(f"{old_of[7]};3;\n{old_of[42]};50;\n{old_of[100]};;\n")


def start():
    p = subprocess.Popen([daemon], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return p, connect("kbd.sock", timeout=5)


proc, s = start()
try:
    # LED index -> sysfs directory, as the daemon numbers them
    r = ask(s, "LIST_LEDS")
    idx = {}
    for line in r.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[0] == "LED":
            idx[int(parts[1])] = parts[2]
    check(len(idx) == NLEDS, f"daemon found the {NLEDS} keyboard LEDs and ignored other LEDs (found {len(idx)})")
    check(all(num(idx[i]) == i for i in range(NLEDS)), "LEDs numbered by name: LED N = rgb:kbd_backlight_N (v1.18.7)")
    csv = open("cfg/keyboard_led_mapping.csv").read()
    check("numbering=name" in csv and "\n7;3;\n" in csv and "\n42;50;\n" in csv and "\n100;;\n" in csv,
          "LED wizard mapping converted once to the name numbering")
    data = json.load(open("cfg/profiles.json"))
    check(data["keyboard_profiles"].get("led_numbering") == "name" and data.get("cpu_freq_profiles") == {"keep": 1},
          "profiles marked as converted, other sections kept")

    def color(i):
        return open(f"leds/{idx[i]}/multi_intensity").read().split()

    def colors():
        return [tuple(int(x) for x in color(i)) for i in range(NLEDS)]

    c = colors()
    check(c[0] == (10, 20, 30) and c[1] == (10, 20, 30) and c[2] == (0, 0, 0),
          f"active profile restored at daemon start (LED0={c[0]}, LED2={c[2]})")

    # keyboard brightness (v1.20.0)
    def bright(i):
        return int(open(f"leds/{idx[i]}/brightness").read())

    check(all(bright(i) == 10 for i in range(NLEDS)), "no saved brightness -> full brightness (10) at start")
    r = ask(s, "SET_BRIGHTNESS 4")
    check(r.startswith("OK") and bright(0) == 4 and bright(119) == 4, "SET_BRIGHTNESS sets the keyboard brightness")
    check("BRIGHTNESS 4 MAX 10" in ask(s, "GET_BRIGHTNESS") and "BRIGHTNESS=4\nBRIGHTNESS_MAX=10" in ask(s, "STATUS"),
          "GET_BRIGHTNESS and STATUS report level and maximum")
    check(open("state/kbd_brightness").read().strip() == "4", "brightness remembered for the next start")
    check(ask(s, "SET_BRIGHTNESS 99").startswith("OK") and bright(0) == 10, "too high a level is limited to the maximum")
    check(ask(s, "SET_BRIGHTNESS x").startswith("ERROR"), "invalid brightness rejected")
    wr(f"leds/{idx[0]}/brightness", "7\n")     # like the desktop's keyboard slider (UPower)
    time.sleep(1.6)
    check(open("state/kbd_brightness").read().strip() == "7" and "BRIGHTNESS 7" in ask(s, "GET_BRIGHTNESS"),
          "brightness changed outside the daemon is noticed and remembered")

    r = ask(s, "SET_KEYS_COLOR::255,255,255::all")
    check(r.startswith("OK") and set(colors()) == {(255, 255, 255)}, "no selection: whole keyboard white")

    r = ask(s, "SET_KEYS_COLOR::255,0,0::5,6,7")
    c = colors()
    check(r.startswith("OK") and c[5] == c[6] == c[7] == (255, 0, 0) and c[4] == (255, 255, 255),
          "selected keys red, the others stay white")
    z = ask(s, "GET_ZONES")
    check("COUNT=2" in z and "name=#FF0000" in z, "one zone per color (Base + #FF0000)")

    many = [i for i in range(NLEDS) if i not in (5, 6, 7, 50)]
    cmd = "SET_KEYS_COLOR::0,0,255::" + ",".join(map(str, many))
    check(len(cmd) > 256, f"command longer than the old 256-byte limit ({len(cmd)} bytes)")
    r = ask(s, cmd)
    c = colors()
    check(r.startswith("OK") and c[119] == (0, 0, 255) and c[50] == (255, 255, 255) and c[5] == (255, 0, 0),
          "long LED list applied completely (last LED blue, unselected keys unchanged)")

    ask(s, "SET_KEYS_COLOR::0,255,0::5,6,7")
    z = ask(s, "GET_ZONES")
    check("#FF0000" not in z and "name=#00FF00" in z and colors()[6] == (0, 255, 0),
          "recoloring all red keys removes the empty red zone")
    ask(s, "SET_KEYS_COLOR::0,255,0::6,6,6")
    zl = [l for l in ask(s, "GET_ZONES").splitlines() if "#00FF00" in l]
    check(zl and "keys=3 " in zl[0], "repeated LED indices are stored once")

    check(ask(s, "SET_KEYS_COLOR::nope::1").startswith("ERROR"), "invalid color rejected")
    check(ask(s, "SET_KEYS_COLOR::1,2,3::999,-4,x").startswith("ERROR"), "list without valid LEDs rejected")

    # several clients at once, framing (v1.18.5)
    b = connect("kbd.sock")
    check("PONG" in ask(b, "PING") and "PONG" in ask(s, "PING"),
          "a second client does not disconnect the first")
    b.sendall(b"SET_KEYS_COLOR::10,20,30::")
    time.sleep(0.05)
    b.sendall(b"100,101\n")
    time.sleep(0.4)
    check(colors()[100] == (10, 20, 30) and colors()[101] == (10, 20, 30),
          "command split across two writes is executed once complete")
    b.recv(65536)
    r = ask(b, "PING\nPING", newline=True)
    check(r.count("PONG") == 2, "two commands in one write both answered")
    c = connect("kbd.sock")
    check("PONG" in ask(c, "PING", newline=False, wait=0.6), "command without newline answered after a short pause")
    c.close()
    b.close()
    time.sleep(0.2)
    check("PONG" in ask(s, "PING"), "first client still connected after the others left")
    ask(s, "SET_KEYS_COLOR::0,255,0::100,101")

    r = ask(s, "SAVE_PROFILE::Start")
    data = json.load(open("cfg/profiles.json"))
    prof = [p for p in data["keyboard_profiles"]["profiles"] if p["name"] == "Start"][0]
    check(r.startswith("OK") and len(prof["zones"]) == 3, f"profile saved with its 3 color zones ({len(prof['zones'])})")
    check(data.get("cpu_freq_profiles") == {"keep": 1}, "saving keeps the other sections of profiles.json")
    ask(s, "LOAD_PROFILE::Empty")
    check(ask(s, "SAVE_PROFILE::Empty").startswith("OK"), "a profile without colors can be saved")

    ask(s, "LOAD_PROFILE::Start")
    ask(s, "TURN_OFF")
    check(set(colors()) == {(0, 0, 0)}, "TURN_OFF switches every LED off")
    ask(s, "TURN_ON")
    c = colors()
    check(c[6] == (0, 255, 0) and c[119] == (0, 0, 255) and c[50] == (255, 255, 255),
          "TURN_ON restores the per-key colors instead of plain white")
    s.close()
    proc.send_signal(signal.SIGTERM)
    proc.wait(timeout=5)
    check(proc.returncode == 0, "SIGTERM -> clean exit (no sanitizer errors)")

    # restart: the active profile comes back
    proc, s = start()
    time.sleep(0.3)
    c = colors()
    check(c[6] == (0, 255, 0) and c[119] == (0, 0, 255) and c[50] == (255, 255, 255),
          "after a restart the active profile's per-key colors are restored")
    check(open("cfg/keyboard_led_mapping.csv").read() == csv, "no second conversion after a restart")
    check(bright(0) == 7, "after a restart the remembered brightness is restored")
    check("IS_ON=1" in ask(s, "STATUS"), "STATUS reports the backlight as on after restoring a profile")

    # the off state survives a restart (v1.18.6)
    ask(s, "TURN_OFF")
    check(os.path.exists("state/kbd_backlight_off"), "TURN_OFF stores the off state")
    s.close()
    proc.send_signal(signal.SIGTERM)
    proc.wait(timeout=5)
    proc, s = start()
    time.sleep(0.3)
    check(set(colors()) == {(0, 0, 0)} and "IS_ON=0" in ask(s, "STATUS"),
          "switched off -> still off after a restart (not turned on again)")
    ask(s, "SET_KEYS_COLOR::255,0,0::all")
    check(not os.path.exists("state/kbd_backlight_off") and "IS_ON=1" in ask(s, "STATUS"),
          "choosing a color switches the light on and clears the off state")
    ask(s, "TURN_OFF")
    ask(s, "LOAD_PROFILE::Start")
    check(not os.path.exists("state/kbd_backlight_off") and colors()[6] == (0, 255, 0),
          "activating a profile with colors switches the light on")
    s.close()
    proc.send_signal(signal.SIGTERM)
    proc.wait(timeout=5)

    # no colors and not switched off -> white at start (the GUI no longer sends TURN_ON)
    d = json.load(open("cfg/profiles.json"))
    d["keyboard_profiles"]["active_profile_index"] = [p["name"] for p in d["keyboard_profiles"]["profiles"]].index("Empty")
    json.dump(d, open("cfg/profiles.json", "w"))
    proc, s = start()
    time.sleep(0.3)
    check(set(colors()) == {(255, 255, 255)} and "IS_ON=1" in ask(s, "STATUS"),
          "no profile colors -> the daemon starts white by itself")
    s.close()
    proc.send_signal(signal.SIGTERM)
    proc.wait(timeout=5)
finally:
    if proc.poll() is None:
        proc.kill()

sys.exit(check.finish())
