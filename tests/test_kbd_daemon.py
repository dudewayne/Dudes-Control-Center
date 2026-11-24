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
cli = os.path.abspath(sys.argv[3]) if len(sys.argv) > 3 else None   # dcc-kbd-cli (1.32.0)
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
                                            "keys": [old_of[0], old_of[1]]}]},
               # before 1.24.0 "whole keyboard" was a zone "Base"
               {"name": "OldBase", "zones": [{"name": "Base", "r": 1, "g": 2, "b": 3,
                                              "keys": [old_of[i] for i in range(2, NLEDS)]},
                                             {"name": "Red", "r": 255, "g": 0, "b": 0,
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
    check("COUNT=1" in z and "BASE color=255,255,255" in z and "name=#FF0000" in z,
          "whole keyboard = base color, one zone per key color (v1.24.0)")

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
    check(r.startswith("OK") and len(prof["zones"]) == 2 and prof.get("base") == {"r": 255, "g": 255, "b": 255},
          f"profile saved with its 2 color zones and the base color ({len(prof['zones'])}, {prof.get('base')})")
    check(data.get("cpu_freq_profiles") == {"keep": 1}, "saving keeps the other sections of profiles.json")
    r = ask(s, "LOAD_PROFILE::Empty")
    log = open(sorted(os.path.join("logs", f) for f in os.listdir("logs"))[0]).read()
    check(r.startswith("OK") and set(colors()) == {(0, 0, 0)} and "No zones to apply" not in log,
          "activating a profile without colors switches all keys off, no error (v1.24.1)")
    check(ask(s, "SAVE_PROFILE::Empty").startswith("OK"), "a profile without colors can be saved")

    # zone editor (v1.24.0): GET_PROFILE, PREVIEW, RESTORE_ACTIVE, old "Base" zone
    g = ask(s, "GET_PROFILE::OldBase")
    check("BASE color=1,2,3" in g and "name=Red color=255,0,0 keys=2 0 1" in g and "name=Base" not in g,
          "old zone 'Base' read as the base color")
    check(ask(s, "GET_PROFILE::Nope").startswith("ERROR"), "GET_PROFILE of a missing profile -> ERROR")
    ask(s, "LOAD_PROFILE::Start")
    r = ask(s, "PREVIEW::0,0,50::My zone|255,0,0|3,4;Two|0,255,0|4,119")
    c = colors()
    check(r.startswith("OK") and c[3] == (255, 0, 0) and c[4] == (0, 255, 0) and c[119] == (0, 255, 0)
          and c[10] == (0, 0, 50), "PREVIEW shows base color and zones (later zone wins)")
    check("name=My zone" in ask(s, "GET_ZONES") and "BASE color=0,0,50" in ask(s, "GET_ZONES"),
          "PREVIEW replaces the live zones (names with spaces)")
    pj = [p for p in json.load(open("cfg/profiles.json"))["keyboard_profiles"]["profiles"] if p["name"] == "Start"][0]
    check(len(pj["zones"]) == 2 and pj.get("base", {}).get("b") == 255, "PREVIEW saves nothing")
    check(ask(s, "PREVIEW::-::").startswith("OK") and set(colors()) == {(0, 0, 0)},
          "PREVIEW of a profile without colors shows all keys off")
    check(ask(s, "PREVIEW::nope::").startswith("ERROR") and ask(s, "PREVIEW::-::x|1,2|3").startswith("ERROR"),
          "PREVIEW with bad data rejected")
    r = ask(s, "RESTORE_ACTIVE")
    c = colors()
    check(r.startswith("OK") and c[6] == (0, 255, 0) and c[119] == (0, 0, 255) and c[50] == (255, 255, 255),
          "RESTORE_ACTIVE brings back the active profile")
    ask(s, "TURN_OFF")
    ask(s, "PREVIEW::0,0,50::")
    check(set(colors()) == {(0, 0, 0)} and "IS_ON=0" in ask(s, "STATUS"), "PREVIEW keeps a switched-off light off")
    ask(s, "PREVIEW::0,0,50::::on")
    check(set(colors()) == {(0, 0, 50)} and "IS_ON=1" in ask(s, "STATUS"), "PREVIEW ...::on switches the light on")
    ask(s, "TURN_OFF")
    ask(s, "RESTORE_ACTIVE")
    check(set(colors()) == {(0, 0, 0)}, "RESTORE_ACTIVE keeps a switched-off light off")
    ask(s, "TURN_ON")
    ask(s, "PREVIEW::9,9,9::Z|1,1,1|0")
    check(ask(s, "SAVE_PROFILE::Empty").startswith("OK"), "SAVE_PROFILE stores the previewed colors")
    g = ask(s, "GET_PROFILE::Empty")
    check("BASE color=9,9,9" in g and "name=Z color=1,1,1 keys=1 0" in g, "saved preview read back")
    # delete keeps the active profile active (v1.24.0)
    ask(s, "CREATE_PROFILE::Del1")
    ask(s, "LOAD_PROFILE::Start")
    names = lambda: [l.split()[-1] for l in ask(s, "LIST_PROFILES").splitlines() if l.startswith("PROFILE ")]
    act = lambda: [l.split()[-1] for l in ask(s, "LIST_PROFILES").splitlines() if "[*]" in l]
    ask(s, "DELETE_PROFILE::Empty")     # before the active one
    check(act() == ["Start"], f"deleting a profile before the active one keeps it active ({act()})")
    ask(s, "CREATE_PROFILE::Empty")
    ask(s, "DELETE_PROFILE::Del1")

    ask(s, "LOAD_PROFILE::Start")
    ask(s, "TURN_OFF")
    check(set(colors()) == {(0, 0, 0)}, "TURN_OFF switches every LED off")
    ask(s, "TURN_ON")
    c = colors()
    check(c[6] == (0, 255, 0) and c[119] == (0, 0, 255) and c[50] == (255, 255, 255),
          "TURN_ON restores the per-key colors instead of plain white")

    # animated effects (1.35.0): drawn by the daemon's main loop
    r = ask(s, "PREVIEW::0,0,0@rainbow:10::")
    c1 = colors(); time.sleep(0.4); c2 = colors()
    check(r.startswith("OK") and len(set(c1)) == 1 and c1 != c2,
          "Rainbow: all keys one color, changing over time")
    check("BASE color=0,0,0 effect=rainbow:10" in ask(s, "GET_ZONES"), "GET_ZONES shows the base effect")
    check("EFFECTS=1" in ask(s, "STATUS"), "STATUS reports a running effect")
    ask(s, "PREVIEW::0,0,0@wave:10:right::")
    time.sleep(0.15)
    c = colors()
    check(c[0] == c[20] == c[100] and c[0] != c[10], "Wave: one color per column, different across the keyboard")
    ask(s, "PREVIEW::0,0,50::Z|255,0,0@breathing:10|5")
    seen = set()
    for _ in range(12):
        seen.add(colors()[5]); time.sleep(0.05)
    base_ok = colors()[6] == (0, 0, 50)
    check(len(seen) > 2 and all(g == 0 and b == 0 for _, g, b in seen) and base_ok,
          f"Breathing fades only its zone's color ({sorted(seen)[:3]}...)")
    check(ask(s, "PREVIEW::0,0,0@nope::").startswith("ERROR") and
          ask(s, "PREVIEW::-::Z|1,2,3@wave:11|0").startswith("ERROR"), "invalid effect or speed rejected")
    ask(s, "CREATE_PROFILE::Fx")
    ask(s, "PREVIEW::1,2,3@wave:4:up::Z|9,9,9@breathing:2|0,1")
    ask(s, "SAVE_PROFILE::Fx")
    g = ask(s, "GET_PROFILE::Fx")
    pj = [p for p in json.load(open("cfg/profiles.json"))["keyboard_profiles"]["profiles"] if p["name"] == "Fx"][0]
    check("BASE color=1,2,3 effect=wave:4:up" in g and "color=9,9,9 effect=breathing:2 keys=2" in g and
          pj["base"].get("effect") == "wave:4:up" and pj["zones"][0].get("effect") == "breathing:2",
          "effects saved with the profile and read back")
    ask(s, "LOAD_PROFILE::Start")
    check("effect=" not in ask(s, "GET_ZONES") and "EFFECTS=0" in ask(s, "STATUS"),
          "loading a static profile stops the effects")
    ask(s, "LOAD_PROFILE::Fx")
    check("effect=wave:4:up" in ask(s, "GET_ZONES"), "LOAD_PROFILE brings the saved effects back")
    ask(s, "TURN_OFF")
    time.sleep(0.15)
    check(set(colors()) == {(0, 0, 0)} and "EFFECTS=0" in ask(s, "STATUS"), "no effect frames while the light is off")
    ask(s, "TURN_ON")
    r = ask(s, "SET_EFFECT::rainbow:3")
    check(r.startswith("OK Effect rainbow:3") and "effect=rainbow:3" in ask(s, "GET_ZONES") and
          ask(s, "SET_EFFECT::sparkle").startswith("ERROR"), "SET_EFFECT sets every zone and the base color")
    ask(s, "DELETE_PROFILE::Fx")
    ask(s, "LOAD_PROFILE::Start")

    # dcc-kbd-cli against this daemon (1.32.0)
    if cli:
        def run(*args):
            r = subprocess.run([cli, *args], capture_output=True, text=True, timeout=10)
            return r.returncode, r.stdout + r.stderr
        rc, out = run()
        check(rc == 0 and "Backlight:      on" in out and "Active profile: Start" in out and "LEDs:           120" in out,
              "CLI: status shows backlight, active profile and LED count")
        rc, out = run("--brightness", "3")
        check(rc == 0 and "Brightness 3 / 10" in out and bright(0) == 3, "CLI: --brightness sets the hardware brightness")
        rc, out = run("--color", "orange")
        check(rc == 0 and set(colors()) == {(255, 165, 0)}, "CLI: --color with a name colors the whole keyboard")
        rc, out = run("--color", "#0080ff")
        check(rc == 0 and set(colors()) == {(0, 128, 255)}, "CLI: --color #RRGGBB")
        rc, out = run("--color", "300,0,0")
        check(rc == 1 and "invalid color" in out, "CLI: invalid color rejected without talking to the daemon")
        rc, out = run("--effect", "wave:7:left")
        check(rc == 0 and "effect wave:7:left" in out and "effect=wave:7:left" in ask(s, "GET_ZONES"),
              "CLI: --effect sets an effect for the whole keyboard (v1.35.0)")
        rc, out = run()
        check("Effects:        running" in out, "CLI: status shows a running effect")
        rc, out = run("--effect", "sparkle")
        check(rc == 1, "CLI: unknown effect -> exit code 1")
        rc, out = run("--effect", "wave::x")
        check(rc == 1 and "invalid effect" in out, "CLI: malformed effect rejected without talking to the daemon")
        ask(s, "LOAD_PROFILE::Start")
        rc, out = run("--list-profiles")
        check(rc == 0 and "* Start" in out and "  Empty" in out, "CLI: --list-profiles marks the active profile")
        rc, out = run("--show-profile", "Start")
        check(rc == 0 and "Base color: 255,255,255" in out and "Zones:" in out, "CLI: --show-profile shows base color and zones")
        rc, out = run("--show-profile", "Nope")
        check(rc == 1 and "Error" in out, "CLI: unknown profile -> error and exit code 1")
        rc, out = run("--profile", "Start")
        c = colors()
        check(rc == 0 and c[6] == (0, 255, 0) and c[50] == (255, 255, 255), "CLI: --profile activates the saved colors")
        rc, out = run("--off")
        check(rc == 0 and set(colors()) == {(0, 0, 0)}, "CLI: --off")
        rc, out = run("--on")
        check(rc == 0 and colors()[6] == (0, 255, 0), "CLI: --on restores the colors")
        rc, out = run("--bogus")
        check(rc == 1 and "unknown option" in out, "CLI: unknown option rejected")
        run("--brightness", "7")     # as before (the restart below expects 7)
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

    # active profile without colors and not switched off -> keys off at start
    d = json.load(open("cfg/profiles.json"))
    d["keyboard_profiles"]["active_profile_index"] = [p["name"] for p in d["keyboard_profiles"]["profiles"]].index("Empty")
    json.dump(d, open("cfg/profiles.json", "w"))
    proc, s = start()
    time.sleep(0.3)
    check(set(colors()) == {(0, 0, 0)} and "IS_ON=1" in ask(s, "STATUS"),
          "active profile without colors -> keys stay off at start (v1.24.1)")
    ask(s, "LOAD_PROFILE::Start")
    ask(s, "LOAD_PROFILE::Empty")
    ask(s, "RESTORE_ACTIVE")
    check(set(colors()) == {(0, 0, 0)}, "RESTORE_ACTIVE of a profile without colors keeps the keys off (v1.24.1)")
    s.close()
    proc.send_signal(signal.SIGTERM)
    proc.wait(timeout=5)

    # no keyboard profile at all -> white at start (the GUI no longer sends TURN_ON)
    d = json.load(open("cfg/profiles.json"))
    del d["keyboard_profiles"]
    json.dump(d, open("cfg/profiles.json", "w"))
    proc, s = start()
    time.sleep(0.3)
    check(set(colors()) == {(255, 255, 255)} and "IS_ON=1" in ask(s, "STATUS"),
          "no keyboard profile -> the daemon starts white by itself")
    ask(s, "RESTORE_ACTIVE")
    check(set(colors()) == {(255, 255, 255)}, "RESTORE_ACTIVE without any profile -> white")
    s.close()
    proc.send_signal(signal.SIGTERM)
    proc.wait(timeout=5)
finally:
    if proc.poll() is None:
        proc.kill()

sys.exit(check.finish())
