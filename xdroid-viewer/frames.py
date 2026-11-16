"""MIRROR-1: summarize the viewer's per-frame log (viewer-frames.csv).

Usage: python frames.py [CSV]   (default %LOCALAPPDATA%/XDroid360/viewer-frames.csv)

Each stage's interval between consecutive shown pictures, then the judder
picture: how many display refreshes each picture stayed on screen.
"""
import csv
import os
import statistics
import sys


def pct(values, p):
    s = sorted(values)
    return s[min(len(s) - 1, int(p / 100 * len(s)))] if s else 0


def row(name, values, unit="ms"):
    if not values:
        return
    print(f"  {name:<22} p50 {pct(values, 50):7.2f}  p95 {pct(values, 95):7.2f}"
          f"  max {max(values):7.2f} {unit}")


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.environ["LOCALAPPDATA"], "XDroid360", "viewer-frames.csv")
    with open(path, encoding="utf-8") as f:
        header = f.readline()
        hz = float(header.split()[2]) if header.startswith("# refresh") else 60
        rows = [{k: int(v) for k, v in r.items()} for r in csv.DictReader(f)]
    if len(rows) < 3:
        sys.exit(f"{path}: {len(rows)} frames")
    period = 1000 / hz
    span = (rows[-1]["present_us"] - rows[0]["present_us"]) / 1e6
    phone_span = (rows[-1]["pts_us"] - rows[0]["pts_us"]) / 1e6
    print(f"{len(rows)} pictures shown in {span:.1f} s = {(len(rows) - 1) / span:.1f} fps"
          f" on screen; monitor {hz:.0f} Hz; vsync {sum(not r['tear'] for r in rows)}"
          f" (paced {sum(r.get('paced', 0) for r in rows)})"
          f" / tear {sum(r['tear'] for r in rows)}")
    guard = [r["guard_us"] / 1000 for r in rows if r.get("paced")]
    row("paced guard", guard)
    print(f"decoder drops {rows[-1]['drops'] - rows[0]['drops']}")
    if "audio_target_us" in rows[0]:  # Logs before the audio columns: skip.
        row("audio buffer", [r["audio_buffer_us"] / 1000 for r in rows])
        row("audio target", [r["audio_target_us"] / 1000 for r in rows])
        print(f"audio underruns {rows[-1]['audio_underruns'] - rows[0]['audio_underruns']}"
              f", overflows {rows[-1]['audio_overflows'] - rows[0]['audio_overflows']}"
              f"; low latency on {sum(r['audio_low_latency'] for r in rows)}"
              f" of {len(rows)} pictures")

    def deltas(key):
        return [(b[key] - a[key]) / 1000 for a, b in zip(rows, rows[1:])]

    pts = deltas("pts_us")
    frame = statistics.median(pts)
    print(f"phone frame interval median {frame:.2f} ms ({1000 / frame:.1f} fps)")
    gaps = [d for d in pts if d > 1.5 * frame]
    lost = sum(round(d / frame) - 1 for d in gaps)
    print(f"pts gaps > 1.5 frames: {len(gaps)}, pictures missing {lost}"
          f" ({lost / (len(rows) + lost) * 100:.1f}%); phone span {phone_span:.1f} s")
    print("intervals between shown pictures:")
    for key in ("pts_us", "aux_us", "receipt_us", "decoded_us", "present_us"):
        row(key[:-3], deltas(key))
    print("stage times:")
    row("encode (phone)", [(r["aux_us"] - r["pts_us"]) / 1000 for r in rows])
    row("transfer", [(r["receipt_us"] - r["aux_us"] + r["offset_us"]) / 1000
                     for r in rows])
    row("decode", [(r["decoded_us"] - r["receipt_us"]) / 1000 for r in rows])
    row("decoded to present", [(r["present_us"] - r["decoded_us"]) / 1000
                               for r in rows])
    row("phone to present", [(r["present_us"] - r["pts_us"] + r["offset_us"]) / 1000
                             for r in rows])

    # Refreshes each picture stayed on screen (vsync: present lands on the
    # next vblank). Even content wants a constant count; 30 fps on 60 Hz = 2.
    held = [round(d / period) for d in deltas("present_us")]
    counts = {}
    for h in held:
        counts[h] = counts.get(h, 0) + 1
    ideal = frame / period
    print(f"refreshes per picture (ideal {ideal:.2f}): " +
          ", ".join(f"{k}x{v}" for k, v in sorted(counts.items())))
    # Judder: on-screen time error vs the phone's own cadence, per picture.
    err = [abs(p - d) for p, d in zip(pts, deltas("present_us"))]
    row("|screen - phone| dt", err)
    worst = sorted(range(len(err)), key=err.__getitem__)[-5:]
    print("worst 5 (index: phone dt / screen dt ms):",
          "; ".join(f"{i}: {pts[i]:.1f}/{deltas('present_us')[i]:.1f}"
                    for i in sorted(worst)))


if __name__ == "__main__":
    main()
