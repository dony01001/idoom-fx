/*
 * IDOOM — trigger/gate effects processor for Schwung / Ableton Move
 *
 * A clone of / homage to the Mystic Circuits IDUM Eurorack module. IDUM
 * is a trademark of Mystic Circuits; IDOOM is unaffiliated. This is an
 * INDEPENDENT reimplementation written from the observed behaviour of the
 * module (manual + firmware study), not a copy of its source: the engine,
 * the break rhythm bank and the rotate scramble matrix are our own, so the
 * module stays MIT-licensed. See README "Engine fidelity & licensing".
 *
 * Incoming MIDI notes are treated as triggers on 4 virtual channels
 * (vchan = note % 4, matching IDUM's TR1-TR4). A clocked step engine
 * (1/16 steps, internal BPM or external MIDI clock) probabilistically
 * activates "modifications" that last 1-8 steps:
 *
 *   #1 HOLD       lengthen note-offs (param right) / skip notes (param left)
 *   #2 BURST      ratchet at a multiple/division of the clock
 *   #3 MULT/DIV   ratchet at a multiple/division of the inter-note interval
 *   #4 BALL       unquantized bouncing-ball ratchet (expand/contract)
 *   #5 ROTATE     scramble the 4 virtual channels via a lookup matrix
 *   #6 GATEDELAY  delay notes by a % of each channel's inter-note interval
 *   #7 BREAK      8 preset rhythm masks (2x resolution, ratchet cells)
 *   #8 SKIP       (adapted) stutter latched notes / mute steps
 *
 * Engine fidelity features (reimplemented mechanisms, our own code):
 *   - probabilityModifier: after a modification, re-trigger is suppressed
 *     for `length` steps so Chance reflects the real % of time modified.
 *   - choke: BREAK only plays a channel once it gets a note in the mod.
 *   - carryOver: back-to-back modifications transition without a dead step.
 *   - looper: records up to LOOP_SUBTICKS triggers per step plus the
 *     modification state per step, re-rolling Chance on playback.
 *
 * Realtime rules: no allocation, no file I/O, no logging in
 * process_midi()/tick(). Every emitted note-on is refcounted so a
 * matching note-off is guaranteed (flush on mode-off/loop/stop).
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "host/midi_fx_api_v1.h"
#include "host/plugin_api_v1.h"

#define N_VCHAN          4
#define LOOP_STEPS       8
#define LOOP_SUBTICKS    4      /* triggers captured per step per channel */
#define BREAK_CELLS      16     /* break runs at 2x clock resolution */
#define EVQ_SIZE         512
#define EVQ_RESERVE      96     /* keep this many slots free for note-offs */
#define CLOCKS_PER_STEP  6      /* 1/16 at 24 PPQN */
#define DEFAULT_BPM      120
#define MIN_PERIOD_MS    8.0    /* fastest ratchet period */

typedef enum {
    M_OFF = 0, M_HOLD, M_BURST, M_MULTDIV, M_BALL,
    M_ROTATE, M_GATEDELAY, M_BREAK, M_SKIP
} idum_mode_t;

typedef enum { SYNC_INTERNAL = 0, SYNC_CLOCK } sync_mode_t;
typedef enum { RES_ODD = 0, RES_EVEN, RES_POW2 } res_mode_t;

static const char *MODE_NAMES[] = {
    "off", "hold", "burst", "multdiv", "ball",
    "rotate", "gatedelay", "break", "skip"
};
#define N_MODES 9

/* Input-note bookkeeping states (for matching note-offs) */
#define IN_PASS      0
#define IN_SUPPRESS  1
#define IN_REMAP     2
#define IN_DELAY     3

/*
 * BREAK rhythm bank — our own patterns (not copied from any firmware).
 * 8 presets x 4 channels x 16 cells. The 16 cells run at 2x the clock
 * resolution (2 cells per 1/16 step). Cell values: 0 = silent, 1 = trigger,
 * >=2 = ratchet (that many sub-triggers across the half-step).
 * Channel intent: 0 = kick, 1 = snare, 2 = hat, 3 = perc.
 */
#define N_BREAK 8
static const uint8_t BREAK_OWN[N_BREAK][N_VCHAN][BREAK_CELLS] = {
  /* 0 — four on the floor */
  { { 1,0,0,0, 1,0,0,0, 1,0,0,0, 1,0,0,0 },
    { 0,0,0,0, 1,0,0,0, 0,0,0,0, 1,0,0,0 },
    { 1,0,1,0, 1,0,1,0, 1,0,1,0, 1,0,1,0 },
    { 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0 } },
  /* 1 — basic break */
  { { 1,0,0,0, 0,0,1,0, 0,0,1,0, 0,0,0,0 },
    { 0,0,0,0, 1,0,0,0, 0,0,0,0, 1,0,0,0 },
    { 1,0,1,0, 1,0,1,0, 1,0,1,0, 1,0,1,0 },
    { 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,1,0 } },
  /* 2 — amen-ish */
  { { 1,0,0,0, 0,0,1,0, 0,0,0,0, 1,0,0,0 },
    { 0,0,0,0, 1,0,0,1, 0,0,1,0, 0,0,1,0 },
    { 2,0,2,0, 2,0,2,0, 2,0,2,0, 2,0,4,0 },
    { 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0 } },
  /* 3 — half-time */
  { { 1,0,0,0, 0,0,0,0, 1,0,0,1, 0,0,0,0 },
    { 0,0,0,0, 0,0,0,0, 1,0,0,0, 0,0,0,0 },
    { 1,0,0,1, 0,0,1,0, 1,0,0,1, 0,0,1,0 },
    { 0,0,1,0, 0,0,0,0, 0,0,1,0, 0,0,0,0 } },
  /* 4 — rolling */
  { { 1,0,0,1, 0,0,1,0, 0,1,0,0, 1,0,0,1 },
    { 0,0,0,0, 1,0,0,0, 0,0,0,0, 1,0,0,0 },
    { 2,2,2,2, 2,2,2,2, 2,2,2,2, 2,2,4,4 },
    { 0,0,0,0, 0,0,1,0, 0,0,0,0, 0,0,1,0 } },
  /* 5 — syncopated */
  { { 1,0,0,1, 0,0,0,1, 0,0,1,0, 0,1,0,0 },
    { 0,0,0,0, 1,0,0,0, 0,0,0,0, 1,0,1,0 },
    { 1,0,1,0, 1,0,1,1, 1,0,1,0, 1,0,1,1 },
    { 0,0,0,0, 0,0,0,0, 1,0,0,0, 0,0,0,0 } },
  /* 6 — dense / glitch */
  { { 1,0,1,0, 0,0,1,0, 1,0,0,0, 1,0,0,0 },
    { 0,0,0,0, 1,0,0,0, 0,1,0,0, 1,0,0,1 },
    { 4,0,2,0, 1,0,2,0, 4,0,2,0, 1,0,4,4 },
    { 0,0,0,1, 0,0,1,0, 0,0,1,1, 0,0,1,0 } },
  /* 7 — build / fill */
  { { 1,0,0,0, 1,0,0,0, 1,0,0,0, 4,4,4,4 },
    { 0,0,0,0, 1,0,1,0, 1,0,1,0, 2,2,2,2 },
    { 1,1,1,1, 1,1,1,1, 2,2,2,2, 4,4,4,4 },
    { 0,0,1,0, 0,0,1,0, 0,0,1,0, 1,1,1,1 } },
};

/*
 * ROTATE scramble matrix — our own. ROT_FWD[idx][in_vchan] = out_vchan.
 * idx 0..7 chosen from the Param knob; index 4 (centre) is identity.
 */
static const uint8_t ROT_FWD[8][N_VCHAN] = {
    { 0, 1, 2, 3 },   /* 0 identity            */
    { 1, 2, 3, 0 },   /* 1 rotate +1           */
    { 2, 3, 0, 1 },   /* 2 rotate +2           */
    { 3, 0, 1, 2 },   /* 3 rotate +3           */
    { 0, 1, 2, 3 },   /* 4 identity (centre)   */
    { 0, 3, 2, 1 },   /* 5 swap 1<->3          */
    { 2, 1, 0, 3 },   /* 6 swap 0<->2          */
    { 3, 2, 1, 0 },   /* 7 reverse             */
};

/* GATEDELAY asymmetric divisors per channel (right side / left side). */
static const int DELAY_DIV_R[N_VCHAN] = { 8, 12, 14, 16 };
static const int DELAY_DIV_L[N_VCHAN] = { 16, 14, 12, 8 };

/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t t;
    uint8_t  msg[3];
    uint8_t  used;
} ev_t;

typedef struct {
    int      active;
    uint64_t next_t;
    double   period;     /* samples between hits */
    double   factor;     /* period *= factor each hit (BALL); 1.0 otherwise */
    uint8_t  note, vel, ch;
} ratchet_t;

typedef struct {
    /* parameters */
    int mode, chance, param, length, loop_on, bpm, sync, res, drift;

    /* time base */
    uint64_t now;             /* sample clock, advanced each tick */
    int      sample_rate;
    double   step_period;     /* samples per 1/16 step */
    double   samples_to_step; /* internal-sync countdown */
    int      clock_count;     /* 0xF8 counter */
    int      clock_running;
    uint64_t last_clock_t;
    double   clock_interval;  /* smoothed samples per 0xF8 */

    /* active modification */
    int mod_active;
    int mod_steps_left;
    int mod_steps_total;
    int mod_step_idx;         /* steps elapsed inside the modification */
    int lmode, lparam;        /* locked at activation */
    int mute_step;            /* SKIP left: current step is muted */
    int prob_mod;             /* probabilityModifier: suppresses re-trigger */

    /* ROTATE index (locked at activation) */
    int rot_idx;

    /* BREAK state */
    int break_preset;
    int break_pos[N_VCHAN];   /* 0..BREAK_CELLS-1 */
    int break_latched[N_VCHAN];  /* choke: set by a note-on during the mod */

    /* per-virtual-channel input tracking */
    uint64_t vc_last_on[N_VCHAN];
    double   vc_interval[N_VCHAN];
    uint8_t  vc_note[N_VCHAN], vc_vel[N_VCHAN], vc_ch[N_VCHAN];
    int      vc_has[N_VCHAN];

    ratchet_t rat[N_VCHAN];

    /* input note state, per MIDI channel + note (off matching) */
    uint8_t  in_state[16][128];
    uint8_t  in_remap[16][128];
    uint32_t in_delay[16][128];

    /* emitted note-on refcounts (hung-note safety) */
    uint8_t emitted[16][128];
    int     flush_req;

    /* scheduled event queue */
    ev_t evq[EVQ_SIZE];
    int  evq_n;               /* number of slots currently used */

    /* looper: ring of last 8 steps; up to LOOP_SUBTICKS triggers per
     * vchan per step, plus the modification state captured per step. */
    uint8_t lp_note[LOOP_STEPS][N_VCHAN][LOOP_SUBTICKS];
    uint8_t lp_vel[LOOP_STEPS][N_VCHAN][LOOP_SUBTICKS];
    uint8_t lp_ch[LOOP_STEPS][N_VCHAN][LOOP_SUBTICKS];
    int8_t  lp_pmode[LOOP_STEPS];   /* mode active when this step recorded */
    int8_t  lp_pparam[LOOP_STEPS];  /* param/... */
    uint8_t lp_plen[LOOP_STEPS];    /* originalModifyLength */
    uint64_t step_start_t;          /* sample time of the current step */
    int rec_pos;
    int play_idx;
    int prev_loop_on;
    int work_ch;              /* the one MIDI channel IDUM currently drives */

    unsigned rng;
} idum_t;

static const host_api_v1_t *g_host = NULL;

/* ------------------------------------------------------------------ */
/* small utilities                                                     */

static unsigned rng_next(idum_t *in) {
    unsigned x = in->rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    if (!x) x = 0xBADC0DEu;
    in->rng = x;
    return x;
}

static int roll100(idum_t *in) { return (int)(rng_next(in) % 100u); }

/* magnitude 0..100 -> 1..8, quantized per resolution option */
static int quant18(int mag, int res) {
    if (mag < 0) mag = -mag;
    if (mag > 100) mag = 100;
    int v = 1 + (mag * 7 + 50) / 100;
    if (v < 1) v = 1;
    if (v > 8) v = 8;
    if (res == RES_EVEN) {
        if (v > 1 && (v & 1)) v++;           /* 1,2,4,6,8 */
        if (v > 8) v = 8;
    } else if (res == RES_POW2) {
        if (v >= 6) v = 8;                    /* 1,2,4,8 */
        else if (v >= 3) v = 4;
    }
    return v;
}

static double min_period(const idum_t *in) {
    return (double)in->sample_rate * MIN_PERIOD_MS / 1000.0;
}

static void calc_step_period(idum_t *in) {
    if (in->sync == SYNC_CLOCK && in->clock_interval > 0.0) {
        in->step_period = in->clock_interval * CLOCKS_PER_STEP;
    } else {
        int bpm = in->bpm > 0 ? in->bpm : DEFAULT_BPM;
        /* 4 sixteenth steps per beat */
        in->step_period = (double)in->sample_rate * 60.0 / ((double)bpm * 4.0);
    }
    if (in->step_period < 64.0) in->step_period = 64.0;
}

/* ------------------------------------------------------------------ */
/* emission helpers — every note that leaves goes through these        */

static void refcount(idum_t *in, const uint8_t *msg) {
    uint8_t st = msg[0] & 0xF0, ch = msg[0] & 0x0F, n = msg[1] & 0x7F;
    if (st == 0x90 && msg[2] > 0) {
        if (in->emitted[ch][n] < 255) in->emitted[ch][n]++;
    } else if (st == 0x80 || (st == 0x90 && msg[2] == 0)) {
        if (in->emitted[ch][n] > 0) in->emitted[ch][n]--;
    }
}

static int emit(idum_t *in, uint8_t out_msgs[][3], int out_lens[],
                int max_out, int *count,
                uint8_t s, uint8_t d1, uint8_t d2) {
    if (*count >= max_out) return 0;
    out_msgs[*count][0] = s;
    out_msgs[*count][1] = d1;
    out_msgs[*count][2] = d2;
    out_lens[*count] = 3;
    uint8_t m[3] = { s, d1, d2 };
    refcount(in, m);
    (*count)++;
    return 1;
}

static int evq_push(idum_t *in, uint64_t t, uint8_t s, uint8_t d1, uint8_t d2) {
    int is_on = ((s & 0xF0) == 0x90) && (d2 > 0);

    /* Back-pressure: refuse new note-ONs while the queue is filling so we
     * always keep room to deliver the matching note-OFFs. A dropped note-on
     * needs no off, so this can never strand a hung note. */
    if (is_on && in->evq_n >= EVQ_SIZE - EVQ_RESERVE) return 0;

    int slot = -1;
    for (int i = 0; i < EVQ_SIZE; i++) {
        if (!in->evq[i].used) { slot = i; break; }
    }
    if (slot < 0) {
        /* full: only a note-off may steal a queued note-on (never reverse) */
        if ((s & 0xF0) != 0x80) return 0;
        for (int i = 0; i < EVQ_SIZE; i++) {
            if ((in->evq[i].msg[0] & 0xF0) == 0x90) { slot = i; break; }
        }
        if (slot < 0) return 0;
        /* stealing reuses a used slot — evq_n unchanged */
    } else {
        in->evq_n++;
    }
    in->evq[slot].t = t;
    in->evq[slot].msg[0] = s;
    in->evq[slot].msg[1] = d1;
    in->evq[slot].msg[2] = d2;
    in->evq[slot].used = 1;
    return 1;
}

/* drain due events; note-offs first so we never strand an off for an on */
static void evq_drain(idum_t *in, uint8_t out_msgs[][3], int out_lens[],
                      int max_out, int *count) {
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < EVQ_SIZE && *count < max_out; i++) {
            if (!in->evq[i].used || in->evq[i].t > in->now) continue;
            uint8_t st = in->evq[i].msg[0] & 0xF0;
            int is_off = (st == 0x80) ||
                         (st == 0x90 && in->evq[i].msg[2] == 0);
            if ((pass == 0) != (is_off != 0)) continue;
            emit(in, out_msgs, out_lens, max_out, count,
                 in->evq[i].msg[0], in->evq[i].msg[1], in->evq[i].msg[2]);
            in->evq[i].used = 0;
            if (in->evq_n > 0) in->evq_n--;
        }
    }
}

/* queue note-offs for every outstanding emitted note-on; if the queue
 * fills up, keep the refcount and retry on the next tick */
static void flush_emitted(idum_t *in) {
    for (int ch = 0; ch < 16; ch++) {
        for (int n = 0; n < 128; n++) {
            while (in->emitted[ch][n] > 0) {
                if (!evq_push(in, in->now, (uint8_t)(0x80 | ch), (uint8_t)n, 0)) {
                    in->flush_req = 1;
                    return;
                }
                in->emitted[ch][n]--;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* ratchets (BURST / MULTDIV / BALL / SKIP-right share this engine)    */

static void ratchet_start(idum_t *in, int vc, double period, double factor,
                          uint8_t note, uint8_t vel, uint8_t ch) {
    double mp = min_period(in);
    if (period < mp) {
        /* "too fast for IDUM to produce" -> silent, per the manual */
        in->rat[vc].active = 0;
        return;
    }
    in->rat[vc].active = 1;
    in->rat[vc].period = period;
    in->rat[vc].factor = factor > 0.0 ? factor : 1.0;
    in->rat[vc].note = note;
    in->rat[vc].vel = vel;
    in->rat[vc].ch = ch;
    in->rat[vc].next_t = in->now + (uint64_t)period;
}

static void ratchets_stop(idum_t *in) {
    for (int v = 0; v < N_VCHAN; v++) in->rat[v].active = 0;
}

static void ratchets_fire(idum_t *in, uint8_t out_msgs[][3], int out_lens[],
                          int max_out, int *count) {
    for (int v = 0; v < N_VCHAN; v++) {
        ratchet_t *r = &in->rat[v];
        if (!r->active) continue;
        while (r->active && r->next_t <= in->now && *count < max_out) {
            emit(in, out_msgs, out_lens, max_out, count,
                 (uint8_t)(0x90 | r->ch), r->note, r->vel);
            double gate = r->period * 0.5;
            double half_step = in->step_period * 0.5;
            if (gate > half_step) gate = half_step;
            evq_push(in, in->now + (uint64_t)gate,
                     (uint8_t)(0x80 | r->ch), r->note, 0);
            r->next_t += (uint64_t)r->period;
            if (r->factor != 1.0) {
                r->period *= r->factor;
                if (r->period < min_period(in) ||
                    r->period > in->step_period * 8.0) {
                    r->active = 0;
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* modification lifecycle                                              */

/* idx 0..7 from the bipolar Param knob (centre=4). */
static int rot_index(int param) {
    int u = (param + 100) * 15 / 200;   /* 0..15 */
    if (u < 0) u = 0;
    if (u > 15) u = 15;
    return u / 2;                        /* 0..7 */
}

static void mod_activate(idum_t *in, int carry_over) {
    in->mod_active = 1;
    /* length, snapped per the resolution option (odd=1-8, even, pow2) */
    {
        int L = in->length < 1 ? 1 : (in->length > 8 ? 8 : in->length);
        if (in->res == RES_EVEN && L > 1 && (L & 1)) L++;
        else if (in->res == RES_POW2) {
            if (L >= 6) L = 8; else if (L >= 3) L = 4;
        }
        if (L > 8) L = 8;
        in->mod_steps_total = L;
    }
    in->mod_steps_left = in->mod_steps_total;
    in->mod_step_idx = 0;
    in->lmode = in->mode;
    in->lparam = in->param;
    in->mute_step = 0;

    /* Drift: internal modulation (one knob). Each modification re-rolls
     * Param within +/-drift, and at higher settings sometimes re-picks the
     * mode too — the spirit of IDUM's CV inputs without extra UI. */
    if (in->drift > 0) {
        int span = in->drift;                       /* 0..100 */
        int off = (int)(rng_next(in) % (unsigned)(2 * span + 1)) - span;
        int p = in->lparam + off;
        if (p < -100) p = -100;
        if (p > 100) p = 100;
        in->lparam = p;
        /* mode re-pick probability scales with drift (up to ~50%) */
        if ((int)(rng_next(in) % 100u) < in->drift / 2) {
            in->lmode = 1 + (int)(rng_next(in) % (unsigned)(N_MODES - 1)); /* 1..8 */
        }
    }

    if (in->lmode == M_ROTATE) {
        in->rot_idx = rot_index(in->lparam);
    } else if (in->lmode == M_BREAK) {
        int p = (in->lparam + 100) * (N_BREAK - 1) / 200;   /* 0..N_BREAK-1 */
        if (p < 0) p = 0;
        if (p >= N_BREAK) p = N_BREAK - 1;
        in->break_preset = p;
        if (!carry_over) {
            /* choke: a channel stays silent until it gets a note this mod */
            for (int v = 0; v < N_VCHAN; v++) {
                in->break_pos[v] = 0;
                in->break_latched[v] = 0;
            }
        }
    } else if (in->lmode == M_SKIP) {
        if (in->lparam > 0) {
            /* global stutter: ratchet latched notes at clock x m */
            int m = quant18(in->lparam, in->res);
            for (int v = 0; v < N_VCHAN; v++) {
                if (in->vc_has[v]) {
                    ratchet_start(in, v, in->step_period / (double)m, 1.0,
                                  in->vc_note[v], in->vc_vel[v], in->vc_ch[v]);
                }
            }
        }
    }
}

static void mod_deactivate(idum_t *in) {
    in->mod_active = 0;
    in->mute_step = 0;
    ratchets_stop(in);   /* their note-offs are already queued per hit */
}

/* ------------------------------------------------------------------ */
/* looper                                                              */

static void loop_clear_slot(idum_t *in, int step) {
    for (int v = 0; v < N_VCHAN; v++)
        for (int s = 0; s < LOOP_SUBTICKS; s++)
            in->lp_note[step][v][s] = 0;
    in->lp_pmode[step] = M_OFF;
    in->lp_pparam[step] = 0;
    in->lp_plen[step] = 0;
}

/* Schedule the two BREAK cells that fall in the current step, for every
 * latched channel. Everything is queued so it works from both the tick
 * (internal sync) and process_midi (external clock) call paths. */
static void break_emit(idum_t *in) {
    double half = in->step_period * 0.5;
    int p = in->break_preset;
    if (p < 0 || p >= N_BREAK) return;
    for (int h = 0; h < 2; h++) {
        uint64_t base = in->now + (uint64_t)(h * half);
        for (int v = 0; v < N_VCHAN; v++) {
            if (!in->break_latched[v]) continue;
            int cell = BREAK_OWN[p][v][in->break_pos[v] % BREAK_CELLS];
            uint8_t ch = in->vc_ch[v], nt = in->vc_note[v], vl = in->vc_vel[v];
            if (cell == 1) {
                if (evq_push(in, base, (uint8_t)(0x90 | ch), nt, vl))
                    evq_push(in, base + (uint64_t)(half * 0.5),
                             (uint8_t)(0x80 | ch), nt, 0);
            } else if (cell >= 2) {
                double per = half / (double)cell;
                if (per >= min_period(in)) {
                    for (int k = 0; k < cell; k++) {
                        uint64_t t0 = base + (uint64_t)(per * (double)k);
                        if (evq_push(in, t0, (uint8_t)(0x90 | ch), nt, vl))
                            evq_push(in, t0 + (uint64_t)(per * 0.5),
                                     (uint8_t)(0x80 | ch), nt, 0);
                    }
                }
            }
            in->break_pos[v] = (in->break_pos[v] + 1) % BREAK_CELLS;
        }
    }
}

/* Per-step action for the active modification (no length bookkeeping).
 * Runs on the activation step and every continuation step, so the mod
 * stays live through the gap where notes arrive — including length 1. */
static void mod_step_action(idum_t *in) {
    in->mod_step_idx++;
    if (in->lmode == M_BREAK) {
        break_emit(in);
    } else if (in->lmode == M_SKIP && in->lparam < 0) {
        int n = quant18(in->lparam, in->res);
        in->mute_step = ((in->mod_step_idx - 1) & 7) < n;
    }
}

/* replay one looper step (sub-ticks), re-rolling Chance and applying the
 * live mode as a per-step modification to the recorded notes. */
static void loop_play_step(idum_t *in) {
    int L = in->length < 1 ? 1 : (in->length > LOOP_STEPS ? LOOP_STEPS : in->length);
    int start = (in->rec_pos - L + 1 + LOOP_STEPS) % LOOP_STEPS;
    int idx = (start + in->play_idx) % LOOP_STEPS;
    double subdt = in->step_period / (double)LOOP_SUBTICKS;

    int apply = (in->mode != M_OFF) && (in->chance > 1) &&
                ((int)(rng_next(in) % 100u) < in->chance);
    int rparam = in->param;

    for (int s = 0; s < LOOP_SUBTICKS; s++) {
        for (int v = 0; v < N_VCHAN; v++) {
            uint8_t note = in->lp_note[idx][v][s];
            if (!note) continue;
            uint8_t vel = in->lp_vel[idx][v][s];
            uint8_t ch  = in->work_ch >= 0 ? (uint8_t)in->work_ch
                                           : in->lp_ch[idx][v][s];
            uint64_t t0 = in->now + (uint64_t)(s * subdt);

            if (apply) {
                switch (in->mode) {
                    case M_HOLD:
                        if (rparam < 0 && (int)(rng_next(in) % 100u) < -rparam)
                            continue;
                        break;
                    case M_ROTATE: {
                        int nv = ROT_FWD[rot_index(rparam)][v];
                        note = (uint8_t)((int)note - v + nv);
                        break;
                    }
                    case M_GATEDELAY: {
                        int mag = rparam < 0 ? -rparam : rparam;
                        int div = rparam >= 0 ? DELAY_DIV_R[v] : DELAY_DIV_L[v];
                        uint64_t d = (uint64_t)(in->step_period * (double)mag
                                                / (100.0 * (double)div));
                        t0 += d;
                        break;
                    }
                    case M_BURST:
                    case M_MULTDIV:
                    case M_BALL:
                    case M_SKIP: {
                        int m = quant18(rparam, in->res);
                        if (m < 2) m = 2;
                        double per = subdt / (double)m;
                        if (per >= min_period(in)) {
                            for (int k = 0; k < m; k++) {
                                uint64_t tk = t0 + (uint64_t)(per * (double)k);
                                if (evq_push(in, tk, (uint8_t)(0x90 | ch), note, vel))
                                    evq_push(in, tk + (uint64_t)(per * 0.5),
                                             (uint8_t)(0x80 | ch), note, 0);
                            }
                        }
                        continue;
                    }
                    default:   /* M_BREAK in loop: just play recorded notes */
                        break;
                }
            }

            if (evq_push(in, t0, (uint8_t)(0x90 | ch), note, vel))
                evq_push(in, t0 + (uint64_t)(subdt * 0.5),
                         (uint8_t)(0x80 | ch), note, 0);
        }
    }

    in->play_idx = (in->play_idx + 1) % L;
}

/* ------------------------------------------------------------------ */
/* step engine — called every 1/16 step from tick (internal) or        */
/* process_midi (external clock)                                       */

static int do_step(idum_t *in, uint8_t out_msgs[][3], int out_lens[],
                   int max_out) {
    (void)out_msgs; (void)out_lens; (void)max_out;
    calc_step_period(in);
    in->step_start_t = in->now;

    if (in->loop_on) {
        loop_play_step(in);
        return 0;   /* looper output is queued; tick drains it */
    }

    /* advance the record ring (the new slot captures this step's notes) */
    in->rec_pos = (in->rec_pos + 1) % LOOP_STEPS;
    loop_clear_slot(in, in->rec_pos);

    /* probabilityModifier decays one notch per step */
    if (in->prob_mod > 0) in->prob_mod--;

    /* single Chance roll used for both a fresh start and a carry-over */
    int roll = (int)(rng_next(in) % 100u) + in->prob_mod * 9;
    if (roll > 100) roll = 100;
    int want_start = (in->mode != M_OFF) &&
                     ((in->chance >= 100) ||
                      (in->chance > 1 && roll < in->chance));

    if (in->mod_active) {
        /* a continuation step: count down first, then act if still alive.
         * The modification stays active across its whole length (the gaps
         * between steps), so even length 1 affects notes for one full step. */
        in->mod_steps_left--;
        if (in->mod_steps_left <= 0) {
            in->prob_mod = in->mod_steps_total - 1;   /* suppress re-trigger */
            mod_deactivate(in);
            /* carryOver: bridge straight into another modification */
            if (want_start) {
                mod_activate(in, 1 /*carry*/);
                mod_step_action(in);
            }
        } else {
            mod_step_action(in);
        }
    } else if (want_start) {
        mod_activate(in, 0);
        mod_step_action(in);
    }

    /* record the modification state of this step for the looper */
    in->lp_pmode[in->rec_pos]  = (int8_t)(in->mod_active ? in->lmode : M_OFF);
    in->lp_pparam[in->rec_pos] = (int8_t)(in->mod_active ? in->lparam : 0);
    in->lp_plen[in->rec_pos]   = (uint8_t)(in->mod_active ? in->mod_steps_total : 0);

    return 0;
}

/* ------------------------------------------------------------------ */
/* note input                                                          */

static int handle_note_on(idum_t *in, uint8_t ch, uint8_t note, uint8_t vel,
                          uint8_t out_msgs[][3], int out_lens[], int max_out) {
    int count = 0;
    int vc = note & 3;

    /* bookkeeping (interval, latch, looper record) happens for all input */
    if (in->vc_last_on[vc] > 0 && in->now > in->vc_last_on[vc]) {
        in->vc_interval[vc] = (double)(in->now - in->vc_last_on[vc]);
    }
    in->vc_last_on[vc] = in->now;
    in->vc_note[vc] = note;
    in->vc_vel[vc] = vel;
    in->vc_ch[vc] = ch;
    in->vc_has[vc] = 1;

    if (!in->loop_on) {
        double subdt = in->step_period / (double)LOOP_SUBTICKS;
        int sub = 0;
        if (subdt > 0.0 && in->now > in->step_start_t)
            sub = (int)((double)(in->now - in->step_start_t) / subdt);
        if (sub < 0) sub = 0;
        if (sub >= LOOP_SUBTICKS) sub = LOOP_SUBTICKS - 1;
        in->lp_note[in->rec_pos][vc][sub] = note;
        in->lp_vel[in->rec_pos][vc][sub] = vel;
        in->lp_ch[in->rec_pos][vc][sub] = ch;
    }

    if (in->loop_on) {
        /* looper owns the output; live input is consumed */
        in->in_state[ch][note] = IN_SUPPRESS;
        return 0;
    }

    if (in->mod_active && in->lmode == M_SKIP && in->mute_step) {
        in->in_state[ch][note] = IN_SUPPRESS;
        return 0;
    }

    if (!in->mod_active) {
        in->in_state[ch][note] = IN_PASS;
        emit(in, out_msgs, out_lens, max_out, &count, (uint8_t)(0x90 | ch), note, vel);
        return count;
    }

    switch (in->lmode) {
        case M_HOLD:
            if (in->lparam < 0 && roll100(in) < -in->lparam) {
                in->in_state[ch][note] = IN_SUPPRESS;
                return 0;
            }
            in->in_state[ch][note] = IN_PASS;
            emit(in, out_msgs, out_lens, max_out, &count, (uint8_t)(0x90 | ch), note, vel);
            break;

        case M_BURST: {
            in->in_state[ch][note] = IN_PASS;
            emit(in, out_msgs, out_lens, max_out, &count, (uint8_t)(0x90 | ch), note, vel);
            int m = quant18(in->lparam, in->res);
            double per = (in->lparam >= 0) ? in->step_period / (double)m
                                           : in->step_period * (double)m;
            ratchet_start(in, vc, per, 1.0, note, vel, ch);
            break;
        }

        case M_MULTDIV: {
            in->in_state[ch][note] = IN_PASS;
            emit(in, out_msgs, out_lens, max_out, &count, (uint8_t)(0x90 | ch), note, vel);
            double base = in->vc_interval[vc] > 0.0 ? in->vc_interval[vc]
                                                    : in->step_period;
            int m = quant18(in->lparam, in->res);
            double per = (in->lparam >= 0) ? base / (double)m
                                           : base * (double)m;
            ratchet_start(in, vc, per, 1.0, note, vel, ch);
            break;
        }

        case M_BALL: {
            in->in_state[ch][note] = IN_PASS;
            emit(in, out_msgs, out_lens, max_out, &count, (uint8_t)(0x90 | ch), note, vel);
            int m = quant18(in->lparam, in->res);
            double start, factor;
            if (in->lparam >= 0) {       /* expanding: fast -> slow */
                start = in->step_period / (double)(m * 2);
                factor = 1.0 + 0.06 * (double)m;
            } else {                     /* contracting: slow -> fast */
                start = in->step_period * (double)in->mod_steps_total / 4.0;
                factor = 1.0 - 0.05 * (double)m;
            }
            ratchet_start(in, vc, start, factor, note, vel, ch);
            break;
        }

        case M_ROTATE: {
            int nv = ROT_FWD[in->rot_idx][vc] & 3;
            uint8_t out_note = (uint8_t)((int)note - vc + nv);
            in->in_state[ch][note] = IN_REMAP;
            in->in_remap[ch][note] = out_note;
            emit(in, out_msgs, out_lens, max_out, &count,
                 (uint8_t)(0x90 | ch), out_note, vel);
            break;
        }

        case M_GATEDELAY: {
            double base = in->vc_interval[vc] > 0.0 ? in->vc_interval[vc]
                                                    : in->step_period;
            int amt = quant18(in->lparam, in->res);    /* 1..8 */
            int div = in->lparam >= 0 ? DELAY_DIV_R[vc] : DELAY_DIV_L[vc];
            uint32_t d = (uint32_t)(base * (double)amt / (double)div);
            in->in_state[ch][note] = IN_DELAY;
            in->in_delay[ch][note] = d;
            evq_push(in, in->now + d, (uint8_t)(0x90 | ch), note, vel);
            break;
        }

        case M_BREAK:
            /* choke: this channel now plays the mask; its position resets
               so the break pattern restarts from the incoming trigger */
            in->break_pos[vc] = 0;
            in->break_latched[vc] = 1;
            in->in_state[ch][note] = IN_SUPPRESS;
            break;

        case M_SKIP:
            /* SKIP-right: stutter — refresh the latch and join the ratchet */
            in->in_state[ch][note] = IN_PASS;
            emit(in, out_msgs, out_lens, max_out, &count, (uint8_t)(0x90 | ch), note, vel);
            if (in->lparam > 0 && !in->rat[vc].active) {
                int m = quant18(in->lparam, in->res);
                ratchet_start(in, vc, in->step_period / (double)m, 1.0,
                              note, vel, ch);
            }
            break;

        default:
            in->in_state[ch][note] = IN_PASS;
            emit(in, out_msgs, out_lens, max_out, &count, (uint8_t)(0x90 | ch), note, vel);
            break;
    }

    return count;
}

static int handle_note_off(idum_t *in, uint8_t ch, uint8_t note,
                           uint8_t out_msgs[][3], int out_lens[], int max_out) {
    int count = 0;
    uint8_t st = in->in_state[ch][note];
    in->in_state[ch][note] = IN_PASS;

    switch (st) {
        case IN_SUPPRESS:
            return 0;

        case IN_REMAP:
            emit(in, out_msgs, out_lens, max_out, &count,
                 (uint8_t)(0x80 | ch), in->in_remap[ch][note], 0);
            return count;

        case IN_DELAY:
            evq_push(in, in->now + in->in_delay[ch][note],
                     (uint8_t)(0x80 | ch), note, 0);
            return 0;

        default:
            break;
    }

    /* HOLD right: lengthen the gate by a % of the modification length */
    if (in->mod_active && in->lmode == M_HOLD && in->lparam > 0) {
        double total = in->step_period * (double)in->mod_steps_total;
        uint64_t d = (uint64_t)(total * (double)in->lparam / 100.0);
        evq_push(in, in->now + d, (uint8_t)(0x80 | ch), note, 0);
        return 0;
    }

    emit(in, out_msgs, out_lens, max_out, &count, (uint8_t)(0x80 | ch), note, 0);
    return count;
}

/* ------------------------------------------------------------------ */
/* plugin entry points                                                 */

static void *idum_create_instance(const char *module_dir, const char *config_json) {
    (void)module_dir;
    (void)config_json;
    idum_t *in = calloc(1, sizeof(idum_t));
    if (!in) return NULL;

    in->mode = M_OFF;
    in->chance = 100;
    in->param = 0;
    in->length = 1;
    in->loop_on = 0;
    in->bpm = DEFAULT_BPM;
    in->sync = SYNC_INTERNAL;
    in->res = RES_ODD;
    in->drift = 0;
    in->work_ch = -1;
    in->sample_rate = 44100;
    in->rng = 0xC0FFEEu;
    calc_step_period(in);
    in->samples_to_step = in->step_period;
    return in;
}

static void idum_destroy_instance(void *instance) {
    if (instance) free(instance);
}

static int idum_process_midi(void *instance,
                             const uint8_t *in_msg, int in_len,
                             uint8_t out_msgs[][3], int out_lens[],
                             int max_out) {
    idum_t *in = (idum_t *)instance;
    if (!in || in_len < 1 || max_out < 1) return 0;

    uint8_t status = in_msg[0];
    uint8_t st = status & 0xF0;

    /* external clock handling (mirrors the upstream arp: consume clock) */
    if (in->sync == SYNC_CLOCK) {
        if (status == 0xF8) {
            /* measure clock interval against the sample clock */
            if (in->last_clock_t > 0 && in->now > in->last_clock_t) {
                double iv = (double)(in->now - in->last_clock_t);
                in->clock_interval = (in->clock_interval > 0.0)
                                   ? in->clock_interval * 0.8 + iv * 0.2
                                   : iv;
            }
            in->last_clock_t = in->now;
            if (in->clock_running) {
                in->clock_count++;
                if (in->clock_count >= CLOCKS_PER_STEP) {
                    in->clock_count = 0;
                    return do_step(in, out_msgs, out_lens, max_out);
                }
            }
            return 0;
        }
        if (status == 0xFA) {           /* start */
            in->clock_count = 0;
            in->clock_running = 1;
            in->mod_active = 0;
            in->mod_step_idx = 0;
            ratchets_stop(in);
            return 0;
        }
        if (status == 0xFC) {           /* stop */
            in->clock_running = 0;
            mod_deactivate(in);
            in->flush_req = 1;
            return 0;
        }
        if (status == 0xFB) {           /* continue */
            in->clock_running = 1;
            return 0;
        }
    }

    if ((st == 0x90 || st == 0x80) && in_len >= 3) {
        uint8_t ch = status & 0x0F;
        uint8_t note = in_msg[1] & 0x7F;
        uint8_t vel = in_msg[2];

        /* Channel gate: IDUM only transforms ONE channel — the first one
         * it sees a note on — and passes every other channel through
         * untouched, so selecting a different Move track never gets
         * glitched by the effect. The working channel follows from the
         * track's Schwung MIDI In/Out config; no user option needed. */
        if (in->work_ch < 0 && st == 0x90 && vel > 0) {
            in->work_ch = ch;            /* auto-lock on first note */
        }
        if (in->work_ch >= 0 && (int)ch != in->work_ch) {
            out_msgs[0][0] = in_msg[0];
            out_msgs[0][1] = note;
            out_msgs[0][2] = vel;
            out_lens[0] = in_len;
            return 1;                    /* foreign channel: pass through */
        }

        if (st == 0x90 && vel > 0) {
            return handle_note_on(in, ch, note, vel, out_msgs, out_lens, max_out);
        }
        return handle_note_off(in, ch, note, out_msgs, out_lens, max_out);
    }

    /* pass everything else through untouched */
    out_msgs[0][0] = in_msg[0];
    out_msgs[0][1] = in_len > 1 ? in_msg[1] : 0;
    out_msgs[0][2] = in_len > 2 ? in_msg[2] : 0;
    out_lens[0] = in_len;
    return 1;
}

static int idum_tick(void *instance,
                     int frames, int sample_rate,
                     uint8_t out_msgs[][3], int out_lens[],
                     int max_out) {
    idum_t *in = (idum_t *)instance;
    if (!in) return 0;

    in->sample_rate = sample_rate > 0 ? sample_rate : 44100;
    in->now += (uint64_t)(frames > 0 ? frames : 0);

    int count = 0;

    /* loop toggle transitions */
    if (in->loop_on != in->prev_loop_on) {
        in->prev_loop_on = in->loop_on;
        in->play_idx = 0;
        mod_deactivate(in);
        in->flush_req = 1;   /* release anything sounding at the boundary */
    }

    if (in->flush_req) {
        in->flush_req = 0;
        flush_emitted(in);
    }

    /* internal step clock */
    if (in->sync == SYNC_INTERNAL) {
        in->samples_to_step -= (double)frames;
        if (in->samples_to_step <= 0.0) {
            calc_step_period(in);
            in->samples_to_step += in->step_period;
            if (in->samples_to_step <= 0.0) in->samples_to_step = in->step_period;
            count += do_step(in, &out_msgs[count], &out_lens[count],
                             max_out - count);
        }
    }

    ratchets_fire(in, out_msgs, out_lens, max_out, &count);
    evq_drain(in, out_msgs, out_lens, max_out, &count);
    return count;
}

/* ------------------------------------------------------------------ */
/* parameters                                                          */

/* Is the string a plain integer? (LFO/mod sources send enum indices as
 * numbers, while the UI knobs send the option names.) */
static int parse_int_str(const char *val, int *out) {
    if (!val || !val[0]) return 0;
    const char *p = val;
    if (*p == '-' || *p == '+') p++;
    if (!*p) return 0;
    while (*p) {
        if (*p < '0' || *p > '9') return 0;
        p++;
    }
    *out = atoi(val);
    return 1;
}

static int parse_mode(const char *val) {
    int idx;
    if (parse_int_str(val, &idx)) {
        return (idx >= 0 && idx < N_MODES) ? idx : -1;
    }
    for (int i = 0; i < N_MODES; i++) {
        if (strcmp(val, MODE_NAMES[i]) == 0) return i;
    }
    return -1;
}

/* tiny JSON readers (same approach as the upstream arp) */
static int json_get_string(const char *json, const char *key, char *out, int out_len) {
    if (!json || !key || !out || out_len < 1) return 0;
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *pos = strstr(json, search);
    if (!pos) return 0;
    const char *colon = strchr(pos, ':');
    if (!colon) return 0;
    colon++;
    while (*colon == ' ' || *colon == '\t') colon++;
    if (*colon != '"') return 0;
    colon++;
    const char *end = strchr(colon, '"');
    if (!end) return 0;
    int len = (int)(end - colon);
    if (len >= out_len) len = out_len - 1;
    strncpy(out, colon, len);
    out[len] = '\0';
    return len;
}

static int json_get_int(const char *json, const char *key, int *out) {
    if (!json || !key || !out) return 0;
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *pos = strstr(json, search);
    if (!pos) return 0;
    const char *colon = strchr(pos, ':');
    if (!colon) return 0;
    colon++;
    while (*colon == ' ' || *colon == '\t') colon++;
    *out = atoi(colon);
    return 1;
}

static void set_one(idum_t *in, const char *key, const char *val) {
    if (strcmp(key, "mode") == 0) {
        int m = parse_mode(val);
        if (m >= 0) {
            if (m == M_OFF && in->mode != M_OFF) {
                mod_deactivate(in);
                in->flush_req = 1;
            }
            in->mode = m;
        }
    } else if (strcmp(key, "chance") == 0) {
        int v = atoi(val);
        in->chance = v < 0 ? 0 : (v > 100 ? 100 : v);
    } else if (strcmp(key, "param") == 0) {
        int v = atoi(val);
        in->param = v < -100 ? -100 : (v > 100 ? 100 : v);
    } else if (strcmp(key, "length") == 0) {
        int v = atoi(val);
        in->length = v < 1 ? 1 : (v > 8 ? 8 : v);
    } else if (strcmp(key, "loop") == 0) {
        int idx;
        if (parse_int_str(val, &idx)) in->loop_on = idx > 0 ? 1 : 0;
        else in->loop_on = (strcmp(val, "on") == 0) ? 1 : 0;
    } else if (strcmp(key, "bpm") == 0) {
        int v = atoi(val);
        in->bpm = v < 40 ? 40 : (v > 240 ? 240 : v);
    } else if (strcmp(key, "sync") == 0) {
        int idx;
        int want_clock = -1;
        if (parse_int_str(val, &idx)) want_clock = idx > 0 ? 1 : 0;
        else if (strcmp(val, "internal") == 0) want_clock = 0;
        else if (strcmp(val, "clock") == 0) want_clock = 1;
        if (want_clock == 0) {
            in->sync = SYNC_INTERNAL;
        } else if (want_clock == 1) {
            in->sync = SYNC_CLOCK;
            in->clock_count = 0;
            in->clock_running = 1;   /* assume running, like the arp */
        }
    } else if (strcmp(key, "res") == 0) {
        int idx;
        if (parse_int_str(val, &idx)) {
            if (idx >= RES_ODD && idx <= RES_POW2) in->res = idx;
        }
        else if (strcmp(val, "odd") == 0) in->res = RES_ODD;
        else if (strcmp(val, "even") == 0) in->res = RES_EVEN;
        else if (strcmp(val, "pow2") == 0) in->res = RES_POW2;
    } else if (strcmp(key, "drift") == 0) {
        int v = atoi(val);
        in->drift = v < 0 ? 0 : (v > 100 ? 100 : v);
    }
}

static void idum_set_param(void *instance, const char *key, const char *val) {
    idum_t *in = (idum_t *)instance;
    if (!in || !key || !val) return;

    if (strcmp(key, "state") == 0) {
        char s[24];
        int v;
        if (json_get_string(val, "mode", s, sizeof(s))) set_one(in, "mode", s);
        if (json_get_int(val, "chance", &v)) { char b[16]; snprintf(b, sizeof(b), "%d", v); set_one(in, "chance", b); }
        if (json_get_int(val, "param", &v))  { char b[16]; snprintf(b, sizeof(b), "%d", v); set_one(in, "param", b); }
        if (json_get_int(val, "length", &v)) { char b[16]; snprintf(b, sizeof(b), "%d", v); set_one(in, "length", b); }
        if (json_get_string(val, "loop", s, sizeof(s))) set_one(in, "loop", s);
        if (json_get_int(val, "bpm", &v))    { char b[16]; snprintf(b, sizeof(b), "%d", v); set_one(in, "bpm", b); }
        if (json_get_string(val, "sync", s, sizeof(s))) set_one(in, "sync", s);
        if (json_get_string(val, "res", s, sizeof(s))) set_one(in, "res", s);
        if (json_get_int(val, "drift", &v)) { char b[16]; snprintf(b, sizeof(b), "%d", v); set_one(in, "drift", b); }
        return;
    }

    set_one(in, key, val);
}

static int idum_get_param(void *instance, const char *key, char *buf, int buf_len) {
    idum_t *in = (idum_t *)instance;
    if (!in || !key || !buf || buf_len < 1) return -1;

    if (strcmp(key, "mode") == 0)
        return snprintf(buf, buf_len, "%s", MODE_NAMES[in->mode]);
    if (strcmp(key, "chance") == 0)
        return snprintf(buf, buf_len, "%d", in->chance);
    if (strcmp(key, "param") == 0)
        return snprintf(buf, buf_len, "%d", in->param);
    if (strcmp(key, "length") == 0)
        return snprintf(buf, buf_len, "%d", in->length);
    if (strcmp(key, "loop") == 0)
        return snprintf(buf, buf_len, "%s", in->loop_on ? "on" : "off");
    if (strcmp(key, "bpm") == 0) {
        if (in->sync == SYNC_CLOCK) return snprintf(buf, buf_len, "SYNC");
        return snprintf(buf, buf_len, "%d", in->bpm);
    }
    if (strcmp(key, "sync") == 0)
        return snprintf(buf, buf_len, "%s",
                        in->sync == SYNC_CLOCK ? "clock" : "internal");
    if (strcmp(key, "res") == 0)
        return snprintf(buf, buf_len, "%s",
                        in->res == RES_EVEN ? "even" :
                        in->res == RES_POW2 ? "pow2" : "odd");
    if (strcmp(key, "drift") == 0)
        return snprintf(buf, buf_len, "%d", in->drift);

    if (strcmp(key, "error") == 0) {
        if (in->sync != SYNC_CLOCK) { buf[0] = '\0'; return 0; }
        int status = MOVE_CLOCK_STATUS_RUNNING;
        if (g_host && g_host->get_clock_status) status = g_host->get_clock_status();
        else if (!in->clock_running) status = MOVE_CLOCK_STATUS_STOPPED;
        if (status == MOVE_CLOCK_STATUS_UNAVAILABLE)
            return snprintf(buf, buf_len, "Enable MIDI Clock Out in Move settings");
        if (status == MOVE_CLOCK_STATUS_STOPPED)
            return snprintf(buf, buf_len, "Clock out enabled, transport stopped");
        buf[0] = '\0';
        return 0;
    }

    if (strcmp(key, "state") == 0) {
        return snprintf(buf, buf_len,
            "{\"mode\":\"%s\",\"chance\":%d,\"param\":%d,\"length\":%d,"
            "\"loop\":\"%s\",\"bpm\":%d,\"sync\":\"%s\",\"res\":\"%s\",\"drift\":%d}",
            MODE_NAMES[in->mode], in->chance, in->param, in->length,
            in->loop_on ? "on" : "off", in->bpm,
            in->sync == SYNC_CLOCK ? "clock" : "internal",
            in->res == RES_EVEN ? "even" : in->res == RES_POW2 ? "pow2" : "odd",
            in->drift);
    }

    if (strcmp(key, "chain_params") == 0) {
        const char *params = "["
            "{\"key\":\"mode\",\"name\":\"Mode\",\"type\":\"enum\",\"options\":"
                "[\"off\",\"hold\",\"burst\",\"multdiv\",\"ball\",\"rotate\","
                "\"gatedelay\",\"break\",\"skip\"]},"
            "{\"key\":\"chance\",\"name\":\"Chance\",\"type\":\"int\",\"min\":0,\"max\":100,\"step\":1},"
            "{\"key\":\"param\",\"name\":\"Param\",\"type\":\"int\",\"min\":-100,\"max\":100,\"step\":1},"
            "{\"key\":\"length\",\"name\":\"Length\",\"type\":\"int\",\"min\":1,\"max\":8,\"step\":1},"
            "{\"key\":\"loop\",\"name\":\"Loop\",\"type\":\"enum\",\"options\":[\"off\",\"on\"]},"
            "{\"key\":\"bpm\",\"name\":\"BPM\",\"type\":\"int\",\"min\":40,\"max\":240,\"step\":1},"
            "{\"key\":\"sync\",\"name\":\"Sync\",\"type\":\"enum\",\"options\":[\"internal\",\"clock\"]},"
            "{\"key\":\"res\",\"name\":\"Resolution\",\"type\":\"enum\",\"options\":[\"odd\",\"even\",\"pow2\"]},"
            "{\"key\":\"drift\",\"name\":\"Drift\",\"type\":\"int\",\"min\":0,\"max\":100,\"step\":1}"
        "]";
        return snprintf(buf, buf_len, "%s", params);
    }

    return -1;
}

static midi_fx_api_v1_t g_api = {
    .api_version = MIDI_FX_API_VERSION,
    .create_instance = idum_create_instance,
    .destroy_instance = idum_destroy_instance,
    .process_midi = idum_process_midi,
    .tick = idum_tick,
    .set_param = idum_set_param,
    .get_param = idum_get_param
};

midi_fx_api_v1_t *move_midi_fx_init(const host_api_v1_t *host) {
    g_host = host;
    return &g_api;
}
