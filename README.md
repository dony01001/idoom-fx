# IDOOM — Schwung MIDI FX for Ableton Move

**IDOOM** is a clone of the **Mystic Circuits IDUM** Eurorack module,
reimagined as a native, chainable MIDI FX for
[Schwung](https://github.com/charlesvestal/schwung) on the Ableton Move.

IDUM is a *trigger effects processor*: instead of generating patterns, it
mangles the sequences that run through it — probabilistically, in sync with
a clock. IDOOM treats incoming MIDI notes as triggers on four virtual
channels (like IDUM's TR1–TR4) and applies the same eight modification
modes, plus an 8-step looper and a built-in Drift knob.

> Independent reimplementation from the IDUM manual and observed behaviour
> — not derived from Mystic Circuits' firmware. IDUM is a trademark of
> Mystic Circuits; IDOOM is an unaffiliated homage. See [Credits](#credits).

---

## How it maps to the original

| IDUM (Eurorack)       | IDOOM (Move)                                            |
|-----------------------|---------------------------------------------------------|
| TR1–TR4 trigger I/O   | Incoming MIDI notes; virtual channel = `note % 4` (on a drum track, pads 36/37/38/39 = TR1–TR4) |
| CL clock input        | MIDI clock (`sync=clock`) or internal BPM (`sync=internal`); one step = 1/16 |
| CHANCE slider         | `chance` 0–100 % — rolled every clock step              |
| LENGTH slider         | `length` 1–8 — steps each modification lasts            |
| MODE knob             | `mode` — off + the 8 modes below                        |
| PARAM knob            | `param` −100…+100 (bipolar, 0 = noon)                   |
| LOOP button / gate    | `loop` on/off — 8-step looper                           |
| CYCLE switch          | not applicable (no external sequencer to re-sync) — omitted |
| Setup: param/length resolution | `res`: odd / even / pow2 (applies to both)     |

## The 8 modes

1. **hold** — Param right: note-offs are delayed by a % of the modification
   length (longer gates). Param left: incoming notes are probabilistically
   skipped, up to full mute.
2. **burst** — Each incoming note starts a ratchet at a multiple (right,
   ×1–8) or division (left, ÷1–8) of the clock, for the rest of the
   modification. Too-fast settings go silent, like the original.
3. **multdiv** — Ratchet speed comes from the time between the last two
   notes on that virtual channel, multiplied/divided 1–8. Per-channel,
   dynamic.
4. **ball** — Bouncing-ball ratchet, unquantized. Right: starts fast and
   slows down (expanding). Left: starts slow and speeds up (contracting).
   Scales with `length`.
5. **rotate** — Right: rotates the four virtual channels (+1…+3 with wrap).
   Left: swaps them in criss-cross pairs. Notes are remapped within their
   group of four.
6. **gatedelay** — Delays each note by a % of its channel's inter-note
   interval. Right weights the delay toward higher channels, left toward
   lower ones — wonky, off-kilter timing.
7. **break** — 8 preset 16-cell rhythm masks at 2× resolution with ratchet cells (channel intent: 1=kick,
   2=snare, 3=hat, 4=perc). `param` selects the preset. An incoming trigger
   resets that channel's mask position; the mask decides what plays.
8. **skip** *(adapted)* — The original manipulates an external sequencer's
   clock, which doesn't exist on Move. Here: param right = stutter (latched
   notes ratchet at clock ×2–8), param left = mute N of every 8 steps.

## Looper

`loop=on` replays the most recent 8 steps (one trigger per virtual channel
per step, like IDUM's firmware). `length` sets the loop length. `chance`
re-rolls the modification every step (length-1 modifications, per the
manual): hold skips, rotate remaps, burst/multdiv/ball/skip ratchet,
break masks.

## Parameters

| Key      | Range            | Default  |
|----------|------------------|----------|
| `mode`   | off / 8 modes    | off      |
| `chance` | 0–100            | 100      |
| `param`  | −100…+100        | 0        |
| `length` | 1–8              | 1        |
| `loop`   | off / on         | off      |
| `bpm`    | 40–240           | 120      |
| `sync`   | internal / clock | internal |
| `res`    | odd / even / pow2| odd      |
| `drift`  | 0–100            | 0        |

**Channel** keeps IDUM on a single MIDI channel so selecting a different
Move track never gets glitched by the effect. `auto` (default) locks to
the first channel it sees a note on; set `1–16` to pin it to your track's
channel explicitly. Notes on any other channel pass through untouched.

**Drift** is built-in modulation in a single knob: each time a
modification activates it re-rolls Param within ±drift, and at higher
settings sometimes re-picks the Mode too. 0 = static, 100 = chaos. It's
the spirit of IDUM's CV inputs without extra UI — for finer control you
can still use the slot's two LFOs on any parameter.

`sync=clock` needs **MIDI Clock Out** enabled in Move's settings; the
module follows 0xF8/start/stop/continue and shows a warning via the
`error` param when the clock is missing.

## Which channel does IDUM control? (important)

IDUM is a per-channel effect, so **the MIDI In/Out channel configuration
of your Move track and the chain slot decides what it touches.** On a Move
drum rack you typically enable Schwung's MIDI with **Shift + (channel
button)** and set **MIDI Out = ch 1** (or the track's channel). That
channel is what reaches IDUM.

How IDUM responds:

- IDUM transforms exactly **one** MIDI channel and passes every other
  channel through untouched. It never sprays generated MIDI onto a track
  you didn't intend.
- It **auto-locks to the first channel it sees a note on** — no option to
  set, it just follows your track's Schwung MIDI In/Out config. If your
  drum rack sends ch 1, IDUM locks to ch 1; just play.
- **If you change the track's MIDI-Out channel later**, IDUM stays on the
  channel it locked to and ignores the new one (the effect appears to
  "stop"). Re-lock by reloading the module. Likewise, if the slot's
  **Receive Channel** is "All" and you select a different track, only the
  locked channel is affected.

In short: the channel is decided by your track/slot MIDI config, not by a
module setting — keep it consistent and selecting other tracks won't be
affected.

## Baking the output (record IDUM into the sequencer)

Because IDUM emits real MIDI on the track's channel, you can **record its
output straight into Move's sequencer** — a "bake" / freeze workflow:

1. Set up IDUM on the track and dial in a pattern you like.
2. Arm/record the track and let the sequencer capture the transformed
   notes (ratchets, breaks, delays and all).
3. Bypass or remove IDUM — the baked notes now play back on their own and
   can be edited like any recorded pattern.

Great for capturing a happy-accident variation, then chopping or layering
it without the effect running live.

## Build

Requires Docker (any platform) — or set `CROSS_PREFIX` with an
aarch64 toolchain to skip it:

```bash
./build.sh          # → dist/dsp.so + idoom-fx-v<version>-module.tar.gz
```

## Install

```bash
./install.sh        # scp to /data/UserData/schwung/modules/midi_fx/idoom-fx/
ssh root@move.local reboot   # native DSP changes need a reboot
```

Or upload the `.tar.gz` via Schwung Manager (`http://move.local:7700`).

## Usage — two routings, different powers

Schwung offers two ways to route a chain MIDI FX, and they matter a lot
for IDUM (see upstream `docs/ADDRESSING_MOVE_SYNTHS.md`):

### A) Slot with **MIDI FX = Schw+Move** (additive)

The FX output is injected *in addition to* the original pad note — Move's
native track always plays what you press. Platform limitation, by design.

- **Works:** burst, multdiv, ball, gatedelay, skip-right (stutter), looper.
- **Inaudible:** hold (gate length is moot on one-shots; the skip side
  can't mute a note Move already played), rotate (the original still
  sounds), break's masking.
- **break/skip tip:** only virtual channels that have already received a
  note get latched — play all four pads once, then engage the mode.

### B) Slot synth + **MIDI FX = Post** (full IDUM, replacing)

1. Load a Schwung **sound generator** on the chain slot (e.g. an sf2 drum
   kit) and keep Move's native track for that channel empty.
2. Load **IDUM** as the slot's MIDI FX, leave it on **Post** (default).
3. Pads → IDUM → slot synth: the FX output is the *only* thing the synth
   hears, so suppression works — hold-mute, rotate, break masks, skip-left
   step mutes. All 8 modes at full strength.

Either way: start with `chance=100`, `length=1`, `param=0`, pick a
`mode`; lower `chance` to taste, raise `length` for longer mangles, flip
`loop` on to capture and re-mangle the last 8 steps.

## Project structure

```
idoom-fx/
├── module.json        — manifest + chain UI params
├── dsp/idoom.c        — the whole engine (midi_fx_api_v1)
├── vendor/host/       — vendored Schwung API headers
├── Dockerfile         — aarch64 cross-compile env
├── build.sh           — compile + package
├── install.sh         — deploy over SSH
└── README.md
```

## Engine fidelity & licensing

IDOOM is an **independent reimplementation** inspired by the Mystic
Circuits IDUM. The engine was written from the module's *observed
behaviour* (manual + study of the open firmware), not by copying its
source. The break rhythm bank and the rotate scramble matrix are **our
own** designs, so this module stays **MIT-licensed**.

The original IDUM firmware is published under **CC BY-SA 4.0**
([github.com/mysticcircuits/IDUM](https://github.com/mysticcircuits/IDUM)).
We did not copy its code or its data tables — algorithms and techniques
are not copyrightable, the literal tables are, so ours are different.

v0.2.0 reimplements several mechanisms observed in the firmware, in our
own code:

- **Probability modifier** — after a modification ends, re-triggering is
  suppressed for `length` steps (decaying one notch per step). This makes
  the Chance knob reflect the real *percentage of time* a modification is
  active, instead of firing on nearly every step when lengths are long.
  At Chance = 100 modifications run continuously (by design); lower it to
  hear the spacing.
- **Break at 2× resolution with ratchets** — 8 of our own breakbeat
  presets, 16 cells each (two cells per step), where a cell can be a
  single hit or a ratchet (2/4 sub-triggers). Plays the four virtual
  channels as kick / snare / hat / perc.
- **Choke** — in break, a channel stays silent until it receives a note
  during the modification (then it follows the mask from that point).
- **Carry-over** — back-to-back modifications transition without a silent
  step between them.
- **Looper with sub-ticks** — records up to 4 triggers per step per
  channel (denser fills survive) plus the modification state of each
  step, re-rolling Chance on playback.
- **Rotate matrix & asymmetric gate-delay divisors** — our own lookup
  tables for channel scrambling and per-channel delay feel.

## Credits

- Original hardware & concept: [Mystic Circuits IDUM](https://www.mysticcircuits.com)
  (CC BY-SA 4.0 firmware) — the inspiration; this is a clean MIT reimplementation.
- Module framework: [Schwung](https://github.com/charlesvestal/schwung)
  by Charles Vestal; engine skeleton based on its `arp` MIDI FX.
