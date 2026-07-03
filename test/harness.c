/*
 * harness.c — offline test bench for the IDOOM engine.
 *
 * Drives dsp/idoom.c through scripted clock + trigger streams on the host
 * (x86), no Move needed, and prints a per-channel hit grid so the output
 * can be compared against the reference IDUM (e.g. the VCV Rack port) run
 * with the same settings.
 *
 * Build:  cc -O2 -Ivendor test/harness.c -o dist/harness   (from module root)
 * Run:    ./dist/harness
 *
 * Time model: 44100 Hz, 128-frame tick blocks. An external MIDI clock is
 * fed at 24 PPQN; one 1/16 step = 6 clocks. Each step is printed as 4
 * sub-cells so ratchets are visible.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "host/midi_fx_api_v1.h"
#include "host/plugin_api_v1.h"

/* the engine under test (single translation unit, no main of its own) */
#include "../dsp/idoom.c"

#define SR            44100
#define FRAMES        128
#define BPM           120
#define STEPS         16          /* 1/16 steps to simulate */
#define SUBCELLS      4           /* print resolution within a step */
#define CLOCKS_STEP   6           /* 24 PPQN / 4 = 6 clocks per 1/16 */

/* grid[channel][step*SUBCELLS + sub] = number of note-ons that landed there */
static int grid[4][STEPS * SUBCELLS];

static double g_clock_interval;   /* samples per MIDI clock */
static double g_step_samples;

/* a scripted input: note-on at a given step on a given virtual channel.
 * note = 36 + vchan so note%4 == vchan (Move drum-pad convention). */
typedef struct { int step; int vchan; } trig_t;

/* Outputs are timestamped by the shared sample clock g_now: both
 * process_midi and tick can emit, so we bucket every note-on by the
 * current g_now into the print grid. */

static uint64_t g_now;            /* sample clock mirror */
static int g_debug = 0;           /* 1 = print each emitted message */

static void collect(uint8_t out[][3], int *olen, int n) {
    for (int i = 0; i < n; i++) {
        if (g_debug) {
            printf("    [dbg] t=%.2f  %02X %d %d\n",
                   (double)g_now / g_step_samples, out[i][0], out[i][1], out[i][2]);
        }
        if ((out[i][0] & 0xF0) != 0x90 || out[i][2] == 0) continue;
        int note = out[i][1];
        int vch = note & 3;
        int cell = (int)((double)g_now / g_step_samples * SUBCELLS);
        if (cell < 0) cell = 0;
        if (cell >= STEPS * SUBCELLS) cell = STEPS * SUBCELLS - 1;
        grid[vch][cell]++;
    }
    (void)olen;
}

static void run_scenario(const char *title,
                         const char *params[][2], int nparams,
                         const trig_t *trigs, int ntrigs) {
    midi_fx_api_v1_t *api = move_midi_fx_init(NULL);
    void *inst = api->create_instance(NULL, NULL);

    api->set_param(inst, "sync", "clock");   /* step on the fed MIDI clock */
    for (int i = 0; i < nparams; i++)
        api->set_param(inst, params[i][0], params[i][1]);

    memset(grid, 0, sizeof(grid));
    g_now = 0;
    g_step_samples = (double)SR * 60.0 / ((double)BPM * 4.0);
    g_clock_interval = g_step_samples / (double)CLOCKS_STEP;

    uint8_t out[MIDI_FX_MAX_OUT_MSGS][3];
    int olen[MIDI_FX_MAX_OUT_MSGS];

    double next_clock = 0.0;
    int clock_count = 0;
    int held[4] = {0,0,0,0};    /* gates currently open, released mid-step */
    int total_ticks = (int)((STEPS + 1) * g_step_samples / FRAMES);

    /* start transport */
    uint8_t start[1] = { 0xFA };
    int n = api->process_midi(inst, start, 1, out, olen, MIDI_FX_MAX_OUT_MSGS);
    collect(out, olen, n);

    for (int t = 0; t < total_ticks; t++) {
        /* emit any MIDI clocks that fall inside this block */
        while (next_clock < (double)(g_now + FRAMES)) {
            g_now = (uint64_t)next_clock;
            uint8_t clk[1] = { 0xF8 };
            n = api->process_midi(inst, clk, 1, out, olen, MIDI_FX_MAX_OUT_MSGS);
            collect(out, olen, n);

            /* release the previous step's gates (half a step of gate width) */
            if (clock_count % CLOCKS_STEP == 3) {
                for (int i = 0; i < 4; i++) {
                    if (!held[i]) continue;
                    uint8_t off[3] = { 0x80, (uint8_t)(36 + i), 0 };
                    n = api->process_midi(inst, off, 3, out, olen, MIDI_FX_MAX_OUT_MSGS);
                    collect(out, olen, n);
                    held[i] = 0;
                }
            }

            /* on step boundaries, fire any scripted triggers for this step */
            if (clock_count % CLOCKS_STEP == 0) {
                int step = clock_count / CLOCKS_STEP;
                for (int i = 0; i < ntrigs; i++) {
                    if (trigs[i].step != step) continue;
                    uint8_t note = (uint8_t)(36 + trigs[i].vchan);
                    uint8_t on[3]  = { 0x90, note, 100 };
                    n = api->process_midi(inst, on, 3, out, olen, MIDI_FX_MAX_OUT_MSGS);
                    collect(out, olen, n);
                    held[trigs[i].vchan] = 1;
                }
            }
            clock_count++;
            next_clock += g_clock_interval;
        }

        g_now = (uint64_t)((uint64_t)(t + 1) * FRAMES);
        n = api->tick(inst, FRAMES, SR, out, olen, MIDI_FX_MAX_OUT_MSGS);
        collect(out, olen, n);
    }

    /* print */
    printf("\n=== %s ===\n", title);
    printf("params:");
    for (int i = 0; i < nparams; i++) printf(" %s=%s", params[i][0], params[i][1]);
    printf("\ninput:");
    for (int i = 0; i < ntrigs; i++) printf(" ch%d@step%d", trigs[i].vchan, trigs[i].step);
    printf("\n");
    printf("        ");
    for (int s = 0; s < STEPS; s++) printf("%-*d", SUBCELLS, s + 1);
    printf("\n");
    for (int v = 0; v < 4; v++) {
        printf("  TR%d   ", v + 1);
        for (int c = 0; c < STEPS * SUBCELLS; c++)
            putchar(grid[v][c] ? (grid[v][c] > 1 ? '#' : 'X') : '.');
        printf("\n");
    }
    api->destroy_instance(inst);
}

int main(void) {
    printf("IDOOM offline harness — BPM %d, %d steps, external clock\n", BPM, STEPS);
    printf("X = hit, # = ratchet(>1 in cell), . = silent. Each step = %d cells.\n", SUBCELLS);

    printf("NOTE: steps 1-2 are warm-up (a modification must be ACTIVE before a\n");
    printf("      trigger is affected — faithful to IDUM). Read from step 3 on.\n");

    /* ch0 hit every step (warm-up lets a modification be active first) */
    trig_t on_ch0[STEPS]; int n_ch0 = 0;
    for (int s = 1; s < STEPS; s++) { on_ch0[n_ch0].step = s; on_ch0[n_ch0].vchan = 0; n_ch0++; }

    /* ch0 hit every other step */
    trig_t alt_ch0[STEPS]; int n_alt = 0;
    for (int s = 1; s < STEPS; s += 2) { alt_ch0[n_alt].step = s; alt_ch0[n_alt].vchan = 0; n_alt++; }

    /* all four channels every step */
    trig_t on_all[STEPS*4]; int n_all = 0;
    for (int s = 1; s < STEPS; s++)
        for (int v = 0; v < 4; v++) { on_all[n_all].step = s; on_all[n_all].vchan = v; n_all++; }

    { const char *p[][2]={{"mode","off"}};
      run_scenario("passthrough (mode=off)", p, 1, on_ch0, n_ch0); }

    { const char *p[][2]={{"mode","burst"},{"chance","100"},{"param","80"},{"length","4"}};
      run_scenario("burst param+80 (ratchet x)", p, 4, alt_ch0, n_alt); }

    { const char *p[][2]={{"mode","burst"},{"chance","100"},{"param","-60"},{"length","4"}};
      run_scenario("burst param-60 (divide)", p, 4, on_ch0, n_ch0); }

    { const char *p[][2]={{"mode","multdiv"},{"chance","100"},{"param","80"},{"length","4"}};
      run_scenario("multdiv param+80", p, 4, alt_ch0, n_alt); }

    { const char *p[][2]={{"mode","hold"},{"chance","100"},{"param","-100"},{"length","4"}};
      run_scenario("hold param-100 (mute)", p, 4, on_ch0, n_ch0); }

    /* set g_debug=1 before any run_scenario to dump raw emitted MIDI events
     * (authoritative when the ASCII grid collapses simultaneous hits). */
    { const char *p[][2]={{"mode","rotate"},{"chance","100"},{"param","50"},{"length","4"}};
      run_scenario("rotate param+50 (shift)", p, 4, on_all, n_all); }

    { const char *p[][2]={{"mode","rotate"},{"chance","100"},{"param","-50"},{"length","4"}};
      run_scenario("rotate param-50 (swap)", p, 4, on_all, n_all); }

    { const char *p[][2]={{"mode","break"},{"chance","100"},{"param","0"},{"length","8"}};
      run_scenario("break preset ~mid", p, 4, on_all, n_all); }

    { const char *p[][2]={{"mode","gatedelay"},{"chance","100"},{"param","60"},{"length","4"}};
      run_scenario("gatedelay param+60", p, 4, alt_ch0, n_alt); }

    { const char *p[][2]={{"mode","skip"},{"chance","100"},{"param","80"},{"length","4"}};
      run_scenario("skip param+80 (stutter)", p, 4, alt_ch0, n_alt); }

    /* Split mode: each channel rolls Chance independently, so the four
     * channels burst on different steps instead of all together. */
    { const char *p[][2]={{"mode","burst"},{"chance","50"},{"param","70"},{"length","2"},{"split","off"}};
      run_scenario("burst chance50 NON-split (all 4 sync)", p, 5, on_all, n_all); }
    { const char *p[][2]={{"mode","burst"},{"chance","50"},{"param","70"},{"length","2"},{"split","on"}};
      run_scenario("burst chance50 SPLIT (per-channel)", p, 5, on_all, n_all); }

    return 0;
}
