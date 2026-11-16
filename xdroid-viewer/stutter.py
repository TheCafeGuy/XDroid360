"""Where the mirror stutter comes from: per mode and per 60 s window."""
import csv
import os
import sys

path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    os.environ["LOCALAPPDATA"], "XDroid360", "viewer-frames.csv")
with open(path, encoding="utf-8") as f:
    lines = f.readlines()
print("".join(l for l in lines if l[0] == "#"), end="")
rows = [{k: int(v) for k, v in r.items()}
        for r in csv.DictReader(l for l in lines if l[0] != "#")]


def pct(v, p):
    s = sorted(v)
    return s[min(len(s) - 1, int(p / 100 * len(s)))] if s else 0


def line(name, v):
    print(f"  {name:<26} n {len(v):6d}  p50 {pct(v, 50):7.1f}  p90 {pct(v, 90):7.1f}"
          f"  p95 {pct(v, 95):7.1f}  p99 {pct(v, 99):7.1f}  max {max(v):8.1f}")


def mode(r):
    return "tear" if r["tear"] else "paced" if r["paced"] else "vsync"


pairs = list(zip(rows, rows[1:]))
print("== pts interval buckets (phone cadence), ms: count")
b = {}
for a, c in pairs:
    d = (c["pts_us"] - a["pts_us"]) / 1000
    k = ("<12" if d < 12 else "12-21" if d < 21 else "21-29" if d < 29 else
         "29-38" if d < 38 else "38-46" if d < 46 else "46-55" if d < 55 else
         "55-71" if d < 71 else "71-105" if d < 105 else ">105")
    b[k] = b.get(k, 0) + 1
print("  " + ", ".join(f"{k}: {v}" for k, v in b.items()))

print("== phone encoder: aux - pts (present to encoder output), ms")
enc = [(r["aux_us"] - r["pts_us"]) / 1000 for r in rows]
line("all", enc)
for lim in (25, 33, 50, 100):
    print(f"  > {lim} ms: {sum(e > lim for e in enc) / len(enc) * 100:.1f}%")
aux = [(c["aux_us"] - a["aux_us"]) / 1000 for a, c in pairs]
print(f"  aux interval < 5 ms (two out together): {sum(d < 5 for d in aux) / len(aux) * 100:.1f}%"
      f", > 50 ms: {sum(d > 50 for d in aux) / len(aux) * 100:.1f}%")
# Jitter the viewer must absorb: lateness of aux against the best-fit line of
# pts (per picture: aux - pts minus the running minimum of the last 300).
jit = []
for i, e in enumerate(enc):
    lo = min(enc[max(0, i - 300):i + 1])
    jit.append(e - lo)
line("encoder jitter vs min", jit)

print("== per encoder rate (auto-tune arms; 0 = the app's own), ms")
for rate in sorted({r.get("encoder_rate", 0) for r in rows}):
    sel = [(a, c) for a, c in pairs
           if a.get("encoder_rate", 0) == rate == c.get("encoder_rate", 0)]
    if not sel:
        continue
    print(f" rate {rate}:")
    line("present to encoder out", [(c["aux_us"] - c["pts_us"]) / 1000 for a, c in sel])
    line("phone to present",
         [(c["present_us"] - c["pts_us"] + c["offset_us"]) / 1000 for a, c in sel])
    line("cadence error", [abs((c["present_us"] - a["present_us"])
                               - (c["pts_us"] - a["pts_us"])) / 1000 for a, c in sel])
    line("pts interval", [(c["pts_us"] - a["pts_us"]) / 1000 for a, c in sel])

print("== per mode: screen cadence error |screen dt - phone dt| and latency, ms")
for m in ("tear", "vsync", "paced"):
    sel = [(a, c) for a, c in pairs if mode(a) == m and mode(c) == m]
    if not sel:
        continue
    err = [abs((c["present_us"] - a["present_us"]) - (c["pts_us"] - a["pts_us"])) / 1000
           for a, c in sel]
    lat = [(c["present_us"] - c["pts_us"] + c["offset_us"]) / 1000 for a, c in sel]
    wait = [(c["present_us"] - c["decoded_us"]) / 1000 for a, c in sel]
    print(f" {m}: {len(sel)} pictures, bad (> 8 ms) {sum(e > 8 for e in err) / len(err) * 100:.1f}%"
          f", very bad (> 25 ms) {sum(e > 25 for e in err) / len(err) * 100:.1f}%")
    line("cadence error", err)
    line("phone to present", lat)
    line("decoded to present", wait)
    drops = sum(c["drops"] - a["drops"] for a, c in sel)
    print(f"  decoder drops {drops}")

print("== per 60 s window: mode mix, fps, encoder p50/p95, cadence error p95, missing")
t0 = rows[0]["present_us"]
win = {}
for a, c in pairs:
    win.setdefault((c["present_us"] - t0) // 60000000, []).append((a, c))
for k in sorted(win):
    sel = win[k]
    d = [(c["pts_us"] - a["pts_us"]) / 1000 for a, c in sel]
    med = pct(d, 50)
    e = [(c["aux_us"] - c["pts_us"]) / 1000 for a, c in sel]
    err = [abs((c["present_us"] - a["present_us"]) - (c["pts_us"] - a["pts_us"])) / 1000
           for a, c in sel]
    modes = {}
    for a, c in sel:
        modes[mode(c)] = modes.get(mode(c), 0) + 1
    miss = sum(round(x / med) - 1 for x in d if x > 1.5 * med)
    print(f"  {k:2d}: {len(sel):5d} pic, phone {1000 / med:5.1f} fps, enc p50 {pct(e, 50):5.1f}"
          f" p95 {pct(e, 95):6.1f}, err p95 {pct(err, 95):5.1f}, missing {miss:3d},"
          f" drops {sel[-1][1]['drops'] - sel[0][0]['drops']:3d}, "
          + " ".join(f"{m} {n}" for m, n in sorted(modes.items())))
