/*
 * Unit tests for the slicing model, plus helpers that print results for
 * comparison with the Python reference implementation (tests/compare.py).
 *
 *   test_model unit
 *   test_model transients FILE THRESHOLD MIN_GAP_MS      => "start end" lines
 *   test_model sfz BASE_NOTE GAIN_DB TUNE POLYPHONY FADE  < "start end mode" lines
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common.h"
#include "dsp.h"
#include "export.h"
#include "model.h"
#include "sample.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond) do { \
        checks++; \
        if (!(cond)) { \
            failures++; \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        } \
    } while (0)

static int is_partition(const SliceDef* s, uint32_t n, uint32_t frames) {
    if (!n)
        return 1;
    for (uint32_t i = 0; i < n; i++) {
        if (s[i].start >= s[i].end)
            return 0;
        if (i + 1 < n && s[i].end != s[i + 1].start)
            return 0;
    }
    return s[n - 1].end <= frames;
}

static void test_even(void) {
    SliceDef s[MAX_SLICES];
    uint32_t n = slicer_even(1000, 4, s);
    CHECK(n == 4);
    CHECK(s[0].start == 0 && s[0].end == 250);
    CHECK(s[3].start == 750 && s[3].end == 1000);
    CHECK(s[0].mode == PLAY_GATE);
    CHECK(is_partition(s, n, 1000));
    // Python round() is banker's rounding but llround rounds half away from
    // zero: 3 slices of 1000 => 333.33, 666.67 (no halves), same either way
    n = slicer_even(1000, 3, s);
    CHECK(n == 3 && s[1].start == 333 && s[2].start == 667);
    CHECK(slicer_even(0, 4, s) == 0);
    CHECK(slicer_even(100, 500, s) == 100); // Clamped to MAX_SLICES, empty ones dropped
}

static void test_normalize(void) {
    SliceDef in[4] = {{100, 50, 0}, {0, 300, 1}, {200, 600, 2}, {200, 900, 0}};
    SliceDef out[MAX_SLICES];
    uint32_t n = slicer_normalize(in, 4, 1000, 36, out);
    // First slice is invalid (start >= end); the duplicate start moves up by one
    CHECK(n == 3);
    CHECK(out[0].start == 0 && out[0].end == 200 && out[0].mode == 1);
    CHECK(out[1].start == 200 && out[1].end == 201);
    CHECK(out[2].start == 201 && out[2].end == 900);
    CHECK(is_partition(out, n, 1000));

    // Only 2 slices fit above note 126
    SliceDef grid[8];
    slicer_even(800, 8, grid);
    CHECK(slicer_normalize(grid, 8, 800, 126, out) == 2);
    CHECK(slicer_max_slices(36) == 92);
    CHECK(slicer_max_slices(1) == 127);
    CHECK(slicer_max_slices(127) == 1);

    // Last end is clamped to the sample
    SliceDef big[1] = {{10, 5000, 0}};
    CHECK(slicer_normalize(big, 1, 1000, 36, out) == 1 && out[0].end == 1000);
}

static void set_test_sample(Model* m, const char* path, uint32_t frames) {
    Envelope env = {0};
    model_set_sample(m, path, frames, &env);
}

static void test_model_flow(void) {
    Model* m = malloc(sizeof(Model));
    model_init(m);
    SliceParams p = m->params;

    // Default: 4 beats x 1 => 4 slices
    set_test_sample(m, "/a.wav", 4800);
    CHECK(m->count == 4 && !m->manual);

    // Grid of 6
    p.slice_mode = SLICE_GRID;
    p.slice_count = 6;
    model_set_params(m, &p);
    CHECK(m->count == 6 && !m->manual);

    // Edits make the slices manual and stay contiguous
    model_select(m, 2);
    CHECK(m->selected == 2);
    model_set_start(m, 1000);
    CHECK(m->manual);
    CHECK(m->slices[2].start == 1000 && m->slices[1].end == 1000);
    model_set_end(m, 4000); // Limited to next slice end - 1
    CHECK(m->slices[2].end == m->slices[3].start);
    CHECK(m->slices[2].end == 3200 - 1);
    model_set_start(m, 0); // Limited to previous slice start + 1
    CHECK(m->slices[2].start == 801);
    model_set_play_mode(m, PLAY_LOOP);
    CHECK(m->slices[2].mode == PLAY_LOOP);

    // Setting the current values isn't a hand edit
    Model* fresh = malloc(sizeof(Model));
    model_init(fresh);
    set_test_sample(fresh, "/a.wav", 4800);
    model_select(fresh, 1);
    model_set_start(fresh, fresh->slices[1].start);
    model_set_end(fresh, fresh->slices[1].end);
    model_set_play_mode(fresh, fresh->slices[1].mode);
    CHECK(!fresh->manual);
    // A selection made before the sample loads is kept
    model_clear_sample(fresh);
    model_select(fresh, 3);
    CHECK(fresh->selected == 3);
    set_test_sample(fresh, "/c.wav", 4800);
    CHECK(fresh->selected == 3 && fresh->count == 4);
    model_free(fresh);
    free(fresh);
    CHECK(is_partition(m->slices, m->count, 4800));

    // Analysis changes don't discard hand edits
    p.slice_count = 3;
    model_set_params(m, &p);
    CHECK(m->count == 6 && m->manual);

    // Base note change drops slices that don't fit, keeping edits
    p.base_note = 124;
    model_set_params(m, &p);
    CHECK(m->count == 4 && m->manual);
    p.base_note = 36;
    model_set_params(m, &p);
    CHECK(m->count == 4);

    // Detect discards edits
    model_detect(m);
    CHECK(m->count == 3 && !m->manual);

    // Selection is clamped
    model_select(m, 50);
    CHECK(m->selected == 2);
    model_select(m, -3);
    CHECK(m->selected == 0);

    // Reloading the same file keeps the slices
    model_select(m, 1);
    model_set_start(m, 1700);
    set_test_sample(m, "/a.wav", 4800);
    CHECK(m->manual && m->count == 3 && m->slices[1].start == 1700);
    // A different file slices again
    set_test_sample(m, "/b.wav", 9600);
    CHECK(!m->manual && m->count == 3 && m->slices[2].end == 9600);

    // Manual mode: detect keeps the slices
    p.slice_mode = SLICE_MANUAL;
    model_set_params(m, &p);
    model_select(m, 0);
    model_set_end(m, 1000);
    model_detect(m);
    CHECK(m->manual && m->slices[0].end == 1000);

    // Map before sample => applied when the sample loads
    char text[8192];
    size_t len = slicemap_format(m, 0, text, sizeof(text));
    CHECK(len > 0);
    SliceMap map;
    CHECK(slicemap_parse(text, &map) == 0);
    CHECK(map.count == 3 && map.manual && map.frames == 9600 && !strcmp(map.sample, "/b.wav"));
    CHECK(map.slices[0].end == 1000 && map.slices[1].start == 1000);

    Model* m2 = malloc(sizeof(Model));
    model_init(m2);
    model_set_map(m2, &map);
    CHECK(m2->count == 0 && m2->has_pending_map);
    set_test_sample(m2, "/other.wav", 9600); // Different file: map ignored
    CHECK(m2->count == 4 && !m2->manual && m2->has_pending_map);
    set_test_sample(m2, "/b.wav", 9600);
    CHECK(m2->count == 3 && m2->manual && m2->slices[0].end == 1000 && !m2->has_pending_map);

    // Map after sample => applied now
    Model* m3 = malloc(sizeof(Model));
    model_init(m3);
    set_test_sample(m3, "/b.wav", 9600);
    model_set_map(m3, &map);
    CHECK(m3->count == 3 && m3->manual && m3->slices[0].end == 1000);
    // ... and a map for another length is kept for later
    map.frames = 1234;
    model_set_map(m3, &map);
    CHECK(m3->has_pending_map && m3->count == 3);

    // Live format has selection and parses back
    m->selected = 2;
    snprintf(m->last_export, sizeof(m->last_export), "/x/y.sfz");
    len = slicemap_format(m, 1, text, sizeof(text));
    CHECK(strstr(text, "selected=2\n") && strstr(text, "base_note=36\n") && strstr(text, "export=/x/y.sfz\n"));
    CHECK(slicemap_parse(text, &map) == 0 && map.selected == 2);
    CHECK(slicemap_parse("version=2\nsample=/a\nframes=10\n", &map) != 0);

    // Hash names maps by content
    CHECK(slicer_hash("abc", 3) == slicer_hash("abc", 3));
    CHECK(slicer_hash("abc", 3) != slicer_hash("abd", 3));

    // Clearing
    model_clear_sample(m);
    CHECK(m->count == 0 && !m->path[0]);

    model_free(m);
    model_free(m2);
    model_free(m3);
    free(m);
    free(m2);
    free(m3);
}

static void test_dsp(void) {
    static float data[8000];
    for (int i = 0; i < 8000; i++)
        data[i] = 0.5f;
    Sample sample = {8000, 8000, 1, {data, data}};
    SliceTable table = {.count = 2, .source_frames = 8000, .base_note = 36};
    table.slices[0] = (SliceDef){0, 4000, PLAY_ONE_SHOT};
    table.slices[1] = (SliceDef){4000, 8000, PLAY_GATE};
    VoicePool pool;
    memset(&pool, 0, sizeof(pool));
    pool.polyphony = MAX_VOICES;
    float l[256], r[256];

    CHECK(!dsp_note_on(&pool, &table, 35, 100, 0)); // Below base note
    CHECK(!dsp_note_on(&pool, &table, 38, 100, 0)); // No third slice
    CHECK(dsp_note_on(&pool, &table, 36, 127, 10));
    memset(l, 0, sizeof(l));
    memset(r, 0, sizeof(r));
    dsp_process(&pool, &table, &sample, 1.0f, 1.0, l, r, 256);
    CHECK(l[0] == 0.0f && l[9] == 0.0f);              // Before the note offset
    CHECK(l[10] == 0.0f);                              // Fade-in starts at 0
    CHECK(fabsf(l[10 + FADE_FRAMES + 5] - 0.5f) < 1e-6f); // After the fade-in
    uint32_t active[4];
    int64_t pos = dsp_playheads(&pool, &table, &sample, active);
    CHECK(active[0] == 1 && pos > 0 && pos < 4000);

    // One-shot ignores note-off; gate fades out
    dsp_note_off(&pool, &table, 36);
    CHECK(pool.voices[0].playing && !pool.voices[0].fade_out);
    CHECK(dsp_note_on(&pool, &table, 37, 127, 0));
    dsp_note_off(&pool, &table, 37);
    CHECK(pool.voices[1].fade_out == FADE_FRAMES);
    dsp_process(&pool, &table, &sample, 1.0f, 1.0, l, r, 256);
    CHECK(!pool.voices[1].playing);

    // Note 0 / all-off fades everything
    dsp_note_off(&pool, &table, 0);
    dsp_process(&pool, &table, &sample, 1.0f, 1.0, l, r, 256);
    CHECK(!pool.voices[0].playing);

    // Polyphony 1: the new note steals, the old one fades in a spare slot
    dsp_set_polyphony(&pool, 1);
    dsp_note_on(&pool, &table, 36, 127, 0);
    dsp_process(&pool, &table, &sample, 1.0f, 1.0, l, r, 256);
    dsp_note_on(&pool, &table, 37, 127, 0);
    CHECK(pool.voices[0].note == 37 && pool.voices[1].note == 36 && pool.voices[1].fade_out);

    // Table shrinking stops voices of removed slices
    SliceTable small = table;
    small.count = 1;
    dsp_process(&pool, &small, &sample, 1.0f, 1.0, l, r, 256);
    CHECK(!pool.voices[0].playing);

    // Loop keeps playing past the slice end
    dsp_reset(&pool);
    dsp_set_polyphony(&pool, 16);
    table.slices[1].mode = PLAY_LOOP;
    dsp_note_on(&pool, &table, 37, 127, 0);
    for (int i = 0; i < 40; i++)
        dsp_process(&pool, &table, &sample, 1.0f, 2.0, l, r, 256);
    CHECK(pool.voices[0].playing);
}

static void test_sfz(void) {
    SliceDef s[2] = {{0, 100, PLAY_ONE_SHOT}, {100, 200, PLAY_LOOP}};
    SfzOptions o = {-3.0f, 2, 8, 64 / 48000.0, 60};
    char buf[4096];
    size_t len = sfz_build(s, 2, "x.wav", &o, buf, sizeof(buf));
    CHECK(len > 0);
    CHECK(strstr(buf, "set_cc20=66\n") != NULL);
    CHECK(strstr(buf, "lokey=61\nhikey=61\n") != NULL);
    CHECK(strstr(buf, "loop_mode=loop_continuous\nloop_start=100\nloop_end=199\n") != NULL);
    CHECK(strstr(buf, "end=99\n") != NULL);
    CHECK(strstr(buf, "volume=-3.00") != NULL);
}

static int run_unit(void) {
    test_even();
    test_normalize();
    test_model_flow();
    test_dsp();
    test_sfz();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

static int run_transients(const char* path, float threshold, float min_gap) {
    Sample* sample;
    Envelope env;
    if (sample_load(path, 48000, &sample, &env))
        return 1;
    SliceDef s[MAX_SLICES];
    uint32_t n = slicer_transients(&env, sample->source_frames, threshold, min_gap, s);
    for (uint32_t i = 0; i < n; i++)
        printf("%u %u\n", s[i].start, s[i].end);
    sample_free(sample);
    envelope_free(&env);
    return 0;
}

static int run_sfz(int argc, char** argv) {
    if (argc < 7)
        return 2;
    SfzOptions o = {(float)atof(argv[3]), atoi(argv[4]), atoi(argv[5]), atof(argv[6]), atoi(argv[2])};
    SliceDef s[MAX_SLICES];
    uint32_t n = 0;
    unsigned long start, end;
    char mode[16];
    while (n < MAX_SLICES && scanf("%lu %lu %15s", &start, &end, mode) == 3) {
        s[n].start = (uint32_t)start;
        s[n].end = (uint32_t)end;
        s[n].mode = !strcmp(mode, "one-shot") ? PLAY_ONE_SHOT : !strcmp(mode, "loop") ? PLAY_LOOP : PLAY_GATE;
        n++;
    }
    static char buf[65536];
    size_t len = sfz_build(s, n, "sample.wav", &o, buf, sizeof(buf));
    fwrite(buf, 1, len, stdout);
    return len ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc >= 2 && !strcmp(argv[1], "unit"))
        return run_unit();
    if (argc >= 5 && !strcmp(argv[1], "transients"))
        return run_transients(argv[2], (float)atof(argv[3]), (float)atof(argv[4]));
    if (argc >= 2 && !strcmp(argv[1], "sfz"))
        return run_sfz(argc, argv);
    fprintf(stderr, "usage: test_model unit | transients FILE THRESHOLD MIN_GAP | sfz ...\n");
    return 2;
}
