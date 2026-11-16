# AUTO-1 - autonomous bench (design, 2026-11-08)

Goal G1 of PLAN.md section 00. Decision (the maintainer, 2026-11-08): recorded pilot. the maintainer records the menu
route and a 5-8 min flight once; autorun replays the recording with no human input. A run lasts under
10 min. Flights are not frame-exact (guest time follows the wall clock), so the pilot gives comparable
flights, not identical ones; frame-exact A/B stays in GPU-BENCH.

## 1. Native: pad script player and recorder (`xdroid-core/src/main/cpp/xdroid_pad_script.{h,cc}`)

### API
- `PadScriptStart(spec)`: `spec` is a path relative to the storage root (`pad/NAME.pad`), or `stop`.
  Starting a new script stops the running one first.
- `PadRecordSet(on)`: start or stop the recorder.
- `PadScriptShutdown()`: emulator teardown. Stops and joins both threads, releases every input the
  script set.
- `PadNotePasses(passes_x100)`: called where the `Frame stats` line is built, with the window's
  render passes per frame x100. Stored in an atomic; feeds `wait passes` and the recorder comments.

Live keys, parsed in `xdroid_live.cc` ApplyTable next to `pad`: `pad_script = "<spec>"` and
`pad_record = 1|0`. Neither may be logged as an unknown "live marker".

### Clock
One tick = one guest swap. Use the AC6 guest-swap counter (`OnGuestSwap`, `xdroid_ac6_jobs.cc`) when
it is running; otherwise `logging::GetFrameNumber()`. The source is chosen once when a script starts
and logged. Threads poll every 4 ms with `sleep_for` (never spin); wall time is used only for
timeouts.

### Script format (text, one command per line, `#` starts a comment, blank lines ignored)
| Command | Meaning |
|---|---|
| `S d buttons_hex lx ly rx ry lt rt` | After `d` ticks since the previous step, set the full pad state. Recorder output. Axes in [-1,1], triggers in [0,1]. |
| `SPEC[:N]` | Press SPEC (same tokens as the live `pad` key: `A+DPAD_DOWN`, `LY=-1`), hold N ticks (default 4), then release what SPEC set. `Nms` holds N milliseconds. |
| `hold SPEC` / `release SPEC` / `release all` | Set and keep / release. |
| `idle N` | Wait N ticks. |
| `wait title HEX [timeout_s]` | Wait until the running title id equals HEX. |
| `wait passes>=N [timeout_s]`, `wait passes<N [timeout_s]` | Wait on the last Frame-stats window. |
| `wait flight [timeout_s]` | Two consecutive Frame-stats windows with passes/frame >= 60. |
| `loop N` ... `endloop` | Repeat; nesting up to 4 levels. |
| `mark NAME` | Log a marker for host tools. |
| `end` | Stop. |

Default timeout 120 s. A timeout or a parse error aborts: release all, log, stop.

### Log lines (tag `XDROID-PAD`, one line per event, never per `S` step)
- `XDROID-PAD start file=F lines=N` (N commands), then `XDROID-PAD clock=swap|frame for=play|rec`
  at the first tick read (after `wait title`: the swap hook is not set before launch)
- `XDROID-PAD wait-done line=L what=W tick=T frame=F waited_ms=M`
- `XDROID-PAD mark NAME tick=T frame=F`
- `XDROID-PAD abort line=L reason=timeout|parse|stopped what=W`
- `XDROID-PAD done steps=N ticks=T frame=F`
- `XDROID-PAD rec start path=P` / `XDROID-PAD rec saved path=P lines=N ticks=T`

### Recorder
- Waits until the title id is nonzero, writes a header comment (title, build stamp) and
  `wait title <hex> 180`, then starts counting ticks.
- Polls the pad state every 4 ms. Writes an `S` line when buttons change or any axis moves by
  >= 0.02 (values rounded to 2 decimals).
- At each passes/frame regime change (crosses 20 or 60) writes `idle a`, `# passes N tick T`, `idle b`.
  `a` ends at the stats window before the change, `b` runs to the window where the change is seen. To
  anchor a recording, **replace** the `idle b` line with the matching wait (`wait flight` after
  `# passes 60+`); the wait then ends on the same window the recorder used, so later S lines keep their
  timing. Without an anchor the timing is unchanged.
- File `<storage>/pad/rec-<epoch>.pad`, buffered, flushed once per second, closed on stop.

### Rules
- Atomics only on the input path; no lock inside a watchdog region (CLAUDE.md rule 8). A mutex may
  guard thread start/stop only (called from the live-config poll thread).
- Script and user input share the pad state: a user press during a replay mixes in.
- No JNI change, no Kotlin change, no new dependency.

## 2. Host: orchestrator `tools/device/autorun.sh`

`autorun.sh [options] NAME ARM...` with arms in liveab form `NAME:cvar=v,cvar=v`.

Options: `--script FILE` (required), `--warm S` (default 60, counted from flight), `--seg S`
(default 60), `--rounds N` (default 2), `--boot` (one boot per arm: boot-only cvars), `--hot` (no
cooldown gate, for chained hot runs), `--unplugged` or `--plugged` (asserted, else refused),
`--max-min M` (default and cap 10; live arms count it from the flight anchor and get as long again to
reach it, `--boot` counts the whole run), `--deploy`, `--keep-config`, `--dry-run` (print the plan, no adb).

Flow:
1. Preflight, refuse with exit 2 when a gate fails: adb answers; screen awake and not locked;
   battery >= 40%; charge state as asked; build stamp matches; start temperature <= `BENCH_T0` unless
   `--hot` (wait up to 300 s); no other run active (lock file in `scratch/`).
2. Config: pull the maintainer's config to `scratch/autorun/<run>/` and verify it (size, md5). Only then move
   the phone copy aside (rename, never delete) so the app writes fresh defaults (player config:
   tested = shipped, no breadcrumbs). Apply boot arms to the fresh file. A `trap` restores the
   original on every exit path and verifies its md5.
3. Save check: if the recording has a save manifest (`NAME.save.md5`), compare; a mismatch is logged
   as a warning in the ledger (the route may desync).
4. Run: autostart, push the script to `files/xdroid/pad/`, write `pad_script` to live.toml after the
   live config has started. Poll the log every 10 s for `XDROID-PAD` lines: abort means exit 3.
5. Measure: after `wait flight` is done plus `--warm`, run the segments (live arms ABAB through the
   existing `phone.sh liveab` with its flight wait off), or one segment per boot with `--boot`.
6. Hard stop at `--max-min` (re-armed at the flight anchor for live arms): stop the app, restore, mark the
   run aborted.
7. Pull log, sys.log, marks and PPMs; run `liveab.py`; append one row to
   `docs/runs/autorun-ledger.tsv` (date, run id, build, script, arms, verdict, reasons, key metrics).

Exit codes: 0 valid, 2 refused, 3 aborted, 4 invalid.

Recording helpers in `phone.sh`: `padrec start` (autostart plus `pad_record = 1`), `padrec stop NAME`
(`pad_record = 0`, pull the newest recording to `tools/device/pad/NAME.pad`, write the save manifest
`NAME.save.md5`).

`tools/device/pad/` holds the recorded routes (`NAME.pad`, hand-edited to add the `wait flight`
anchor) and their save manifests (`NAME.save.md5`: md5 of every `content/*/00000001` file, taken at
`padrec start`, the state a replay starts from). `phone.sh padrec manifest` prints the same list.

As built (2026-11-08): the swap also moves `config/<TITLE>.config.toml` aside when it exists (title
from the script's `wait title`); `--boot` adds one generation boot that writes the fresh file, then
pushes that copy plus the arm's cvars before every boot; PPMs pulled are the 12 newest of this run
(`AUTORUN_PPM_MAX`); a leftover `*.autorun-*` config on the phone or an active `phone.sh ab` refuses
the run. The save tree (`content/*/00000001`) is backed up to `save.tar` before the first boot and
re-checked at restore (`warn:save_changed`); `--keep-config` still backs the config up and pushes it
back if the app rewrote it (`config_rewritten`); while `scratch/autorun.lock` exists, `phone.sh`
refuses `ab`, `restore`, `deploy` and `cfg set|add|del` from anyone but autorun. RESMISS is not a
per-segment event: its log is capped at 200 lines per process, all at mission load.

## 3. Metrics and validity (`liveab.py`, `logsum.py`)

- Per segment, besides today's columns: audio underruns, XDROID-GLITCH detections (not the dump or
  "saved" lines), XDROID-HUD1, stalls >= 100 ms, RESMISS, torn TEXMISS, rates per minute.
- INVALID reasons: stale log, charge state changed, battery < 30%, crash or pid gone, `XDROID-PAD
  abort`, fewer than 3 stats windows in a segment, plugged when unplugged was asked.
- Bootstrap CI (2000 resamples, paired by round) of each arm's delta vs the first arm. REAL only
  when the 95% CI excludes 0 and the old 2x-spread rule also holds.
- `logsum.py`: audio underrun count and largest 5 s burst, GLITCH detection count, PAD summary.

## 4. Exit (PLAN.md AUTO-1)

Three VALID runs in a row with no human input, and the A/A spread known per metric (AUTO-1f).
