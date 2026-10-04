"""Drives the menu DLL in a real D3D12 swap chain through the harness and checks what it drew.

Covers the three back buffer formats the game can present (8-bit SDR, 10-bit HDR10, float scRGB),
every kind of row worked from the keyboard, a mouse click, picking a key, a quick key pressed with
the menu closed, the text shown for it, and the event file the Lua core reads. F10 and the quick
key go through the DLL's own key check; everything else arrives as window messages, the way ImGui
reads the game's.

Requires: pip install pillow
Run:      python tests/test_overlay.py <ac8overlay_harness.exe> <ac8overlay.dll> [folder to keep PNGs in]
"""
import pathlib
import struct
import subprocess
import sys
import tempfile
import time

from PIL import Image

W, H = 1280, 720
F10, DOWN, RIGHT, SPACE, ENTER, BACKSPACE, CTRL, F7, F8 = 121, 40, 39, 32, 13, 8, 17, 118, 119
SKY = (0.10, 0.20, 0.40)

MENU = """title\tAC8 Tweaks
hdr\t1
nits\t200
ack\t{ack}
key\t121\tF10
hotkey\tcycle\t119
section\tgraphics\tGraphics\t1
row\tadaptive\tswitch\tAdaptive frame generation\t0
row\tboost\tswitch\tReflex boost\t1
row\tscale\tnumber\tScale\t1\t0.25\t0.25\t4\t2\tx
row\ttarget\tnumber\tTarget\t377405\t100000\t0\t99999999\t0\t
section\tmore\tMore\t1
row\tfg\tchoice\tFrame generation\t2\toff|2x|3x|4x
row\treset\taction\tPut everything back\t
row\tview\ttext\tView\tCockpit
row\tkey_fg\tkey\tFrame generation key\tF6
section\thidden\tHidden\t0
row\tunseen\tnumber\tUnseen\t5\t1\t\t\t0\t
"""

# Opens the menu, then walks down it with the arrow keys using every kind of row.
# Numbers are frames; "ch" entries are typed characters.
SCRIPT = [
    (5, F10),                                                   # open
    (9, F8),                                                    # the quick key does nothing while the menu is open
    (15, DOWN), (20, SPACE),                                    # Adaptive frame generation: tick
    (25, DOWN), (30, DOWN), (35, ENTER), "ch:40:50", (45, ENTER),              # Scale: replace 1.00 with 2
    (50, DOWN), (55, ENTER), "ch:58:49", "ch:61:50", (65, ENTER),              # Target: replace 377405 with 12
    (70, DOWN), (75, DOWN), (80, ENTER), (85, DOWN), (90, ENTER),              # Frame generation: 3x to 4x
    (95, DOWN), (100, SPACE),                                   # Put everything back: press
    (105, DOWN), (110, SPACE), (116, CTRL), (117, F7),          # Frame generation key: pick CTRL+F7
    (122, SPACE), (128, BACKSPACE),                             # and again: no key
    (134, SPACE), (140, F10),                                   # and again: F10 is picked, so it does not close the menu
    (146, DOWN), (151, RIGHT),                                  # Hidden group: open
    "m:153:57:158",                                             # mouse: click the Reflex boost box
    (161, F10),                                                 # close
    (170, F8),                                                  # the quick key, now that the menu is closed
]
FRAMES = 180
CAPTURES = [(12, "open"), (63, "typing"), (113, "picking"), (156, "end"), (168, "closed")]
EXPECTED = ["menu\t1", "set\tadaptive\t1", "set\tscale\t2", "set\ttarget\t12", "set\tfg\t3", "press\treset",
            f"set\tkey_fg\t{F7 + 256}", "set\tkey_fg\t0", f"set\tkey_fg\t{F10}", "section\thidden\t1", "set\tboost\t0", "menu\t0", "key\tcycle"]

failures = 0


def check(cond, msg):
    global failures
    if not cond:
        failures += 1
        print(f"    FAIL {msg}")


def pq(nits):
    y = nits / 10000.0
    p = y ** 0.1593017578125
    return ((0.8359375 + 18.8515625 * p) / (1 + 18.6875 * p)) ** 78.84375


def load(path, fmt):
    """Rows of (r, g, b) as fractions of full scale (for fp16, the float values themselves)."""
    data = path.read_bytes()
    if fmt == "rgba8":
        px = [(data[i] / 255, data[i + 1] / 255, data[i + 2] / 255) for i in range(0, len(data), 4)]
    elif fmt == "rgb10":
        words = struct.unpack(f"<{W * H}I", data)
        px = [((v & 1023) / 1023, (v >> 10 & 1023) / 1023, (v >> 20 & 1023) / 1023) for v in words]
    else:
        halves = struct.unpack(f"<{W * H * 4}e", data)
        px = [(halves[i], halves[i + 1], halves[i + 2]) for i in range(0, len(halves), 4)]
    return px


def save_png(px, peak, path):
    img = Image.new("RGB", (W, H))
    img.putdata([tuple(min(255, int(255 * c / peak)) for c in p) for p in px])
    img.save(path)


def is_sky(p):
    return all(abs(c - s) < 0.01 for c, s in zip(p, SKY))


def run(harness, dll, fmt, keep, ack_midway=False):
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        (tmp / "AC8Tweaks").mkdir()
        menu, events = tmp / "AC8Tweaks" / "menu.txt", tmp / "AC8Tweaks" / "menu-events.txt"
        menu.write_text(MENU.format(ack=0))
        keys = [s if isinstance(s, str) else f"{s[0]}:{s[1]}" for s in SCRIPT]
        args = [str(harness), str(dll), fmt, str(tmp), str(FRAMES)] + keys + [f"cap:{f}:{n}" for f, n in CAPTURES]
        proc = subprocess.Popen(args, stdout=subprocess.PIPE, text=True)
        if ack_midway:  # what the Lua core does: acknowledge events, which makes the DLL drop them
            deadline = time.time() + 20
            while time.time() < deadline and proc.poll() is None:
                try:
                    if len(events.read_text().splitlines()) >= 2:
                        break
                except OSError:
                    pass
                time.sleep(0.01)
            menu.write_text(MENU.format(ack=2))
        out = proc.communicate(timeout=60)[0]
        check(proc.returncode == 0 and "done" in out, f"harness failed: {out!r}")
        log = (tmp / "AC8Tweaks" / "overlay.log").read_text() if (tmp / "AC8Tweaks" / "overlay.log").exists() else ""
        check("menu ready" in log and "failed" not in log, f"overlay log: {log!r}")
        got = [line.split("\t", 1) for line in events.read_text().splitlines()] if events.exists() else []
        if ack_midway:
            check([int(s) for s, _ in got] == list(range(3, len(EXPECTED) + 1)), f"acknowledged events were kept: {got}")
            return
        check([e for _, e in got] == EXPECTED, f"events: {[e for _, e in got]}")
        check([int(s) for s, _ in got] == list(range(1, len(EXPECTED) + 1)), "event numbers do not count up from 1")

        peak_expected = {"rgba8": (0.9, 1.0), "rgb10": (pq(100), pq(200) + 0.01), "fp16": (1.5, 2.6)}[fmt]
        for _, name in CAPTURES:
            px = load(tmp / f"{name}.raw", fmt)
            if keep:
                save_png(px, peak_expected[1], pathlib.Path(keep) / f"{fmt}-{name}.png")
            changed = sum(not is_sky(p) for p in px)
            if name == "closed":
                check(changed == 0, f"{name}: {changed} pixels drawn while the menu is closed")
                continue
            check(changed > 40000, f"{name}: only {changed} pixels drawn")
            check(is_sky(px[5 * W + 5]) and is_sky(px[(H - 5) * W + W - 5]), f"{name}: the game image outside the menu changed")
            peak = max(max(p) for p in px)
            check(peak_expected[0] <= peak <= peak_expected[1], f"{name}: brightest value {peak:.3f}, expected {peak_expected}")


def run_toast(harness, dll, keep):
    """With the menu closed, a toast line draws a little text and nothing else."""
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        (tmp / "AC8Tweaks").mkdir()
        (tmp / "AC8Tweaks" / "menu.txt").write_text(MENU.format(ack=0) + "toast\tFrame generation 3x\n")
        out = subprocess.run([str(harness), str(dll), "rgba8", str(tmp), "30", "cap:25:toast"], stdout=subprocess.PIPE, text=True, timeout=60)
        check(out.returncode == 0 and "done" in out.stdout, f"harness failed: {out.stdout!r}")
        check(not (tmp / "AC8Tweaks" / "menu-events.txt").exists(), "showing the text is not an event")
        px = load(tmp / "toast.raw", "rgba8")
        if keep:
            save_png(px, 1.0, pathlib.Path(keep) / "rgba8-toast.png")
        changed = sum(not is_sky(p) for p in px)
        check(1000 < changed < 40000, f"toast: {changed} pixels drawn, expected a line of text in a small box")


if __name__ == "__main__":
    harness, dll = pathlib.Path(sys.argv[1]).resolve(), pathlib.Path(sys.argv[2]).resolve()
    keep = sys.argv[3] if len(sys.argv) > 3 else None
    for name, fmt, ack in [("SDR 8-bit", "rgba8", False), ("HDR10", "rgb10", False), ("scRGB", "fp16", False), ("acknowledged events", "rgba8", True)]:
        before = failures
        run(harness, dll, fmt, keep, ack)
        print(f"{'ok  ' if failures == before else 'FAIL'} {name}")
    before = failures
    run_toast(harness, dll, keep)
    print(f"{'ok  ' if failures == before else 'FAIL'} text for a quick key")
    sys.exit(1 if failures else 0)
