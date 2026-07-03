# Testing IDOOM

Two complementary ways to check the engine does what IDUM does — without
owning the hardware.

## 1. Offline harness (objective, no Move)

`test/harness.c` links the engine (`dsp/idoom.c`) and drives it with a
scripted MIDI clock + trigger stream, printing a per-channel hit grid.

```bash
# needs a native (x86) C compiler; via Docker:
docker run --rm -v "$PWD:/build" -w /build gcc:bookworm \
  bash -c "gcc -O2 -Wall -Ivendor test/harness.c -o /tmp/h && /tmp/h"
```

Grid legend: `X` = a hit, `#` = a cell with a ratchet (>1 hit), `.` =
silence. Each 1/16 step is 4 cells. **Steps 1–2 are warm-up** — a
modification must be *active* before it affects a trigger (faithful to
IDUM: "modes are locked in once a modification activates").

Set `g_debug = 1` before a `run_scenario(...)` call to dump every emitted
MIDI message with its timestamp — authoritative when the ASCII grid
collapses simultaneous multi-channel hits into one cell (e.g. rotate).

### What each mode should show
| Scenario | Expected |
|---|---|
| passthrough (off) | input echoed, unchanged |
| burst +80 | dense ratchets on the hit channel |
| burst −60 | sparse (division = fewer/slower hits) |
| multdiv +80 | dense ratchets, speed from the inter-note interval |
| hold −100 | silence (full mute) |
| rotate ±50 | all four notes emitted every step, **reordered** (check the raw dump: notes 36–39 appear permuted) |
| break | selected preset's mask on TR1/TR3 (kick/hat), TR2/TR4 sparser |
| gatedelay +60 | each hit pushed later in time |
| skip +80 | dense stutter of the latched note |

Status (v0.3.3): all of the above verified — rotate confirmed correct via
the raw event dump (the grid's "sparse/identical" look is a rendering
artifact of simultaneous same-cell hits, not an engine fault).

## 2. A/B against the reference IDUM (subjective, your ears)

An official IDUM port exists for **VCV Rack 2** and the **4ms MetaModule**
("Glitch Please — IDUM ported to VCV Rack and MetaModule"). To compare:

1. In VCV Rack, patch a clock → IDUM, and IDUM's trigger outs → scope /
   drum voices. Use the same settings as a harness scenario (mode, Chance
   = 100, Param, Length, clock tempo 120).
2. Feed the same trigger pattern (e.g. a hit every other 1/16 on TR1).
3. Compare the resulting gate pattern to the harness grid / the IDOOM
   output on the Move.

Matching pattern → confidence. A divergence → capture the settings and
open an issue so it can be reproduced in the harness and fixed.

> Note: VCV's port extends Length to 16; IDOOM currently uses 1–8.
