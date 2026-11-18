/*
 * ******************************************************************
 * Zynthian Slicer LV2 plugin: slicing model (worker thread only)
 *
 * ******************************************************************
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or any later version.
 *
 * For a full copy of the GNU General Public License see the LICENSE.txt file.
 *
 * ******************************************************************
 */

#include "model.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char* PLAY_MODE_NAMES[PLAY_MODE_COUNT] = {"one-shot", "gate", "loop"};

const char* play_mode_name(int mode) {
    if (mode < 0 || mode >= PLAY_MODE_COUNT)
        mode = DEFAULT_PLAY_MODE;
    return PLAY_MODE_NAMES[mode];
}

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// ----------------------------------------------------------------------------
// Pure slicing functions
// ----------------------------------------------------------------------------

int slicer_max_slices(int base_note) {
    return clampi(MAX_NOTE - base_note + 1, 0, MAX_SLICES);
}

uint32_t slicer_even(uint32_t frames, int count, SliceDef* out) {
    count = clampi(count, 1, MAX_SLICES);
    if (frames == 0)
        return 0;
    double step = (double)frames / count;
    uint32_t n = 0;
    for (int i = 0; i < count; i++) {
        uint32_t start = (uint32_t)llround(i * step);
        uint32_t end = i < count - 1 ? (uint32_t)llround((i + 1) * step) : frames;
        if (end > start) {
            out[n].start = start;
            out[n].end = end;
            out[n].mode = DEFAULT_PLAY_MODE;
            n++;
        }
    }
    return n;
}

typedef struct {
    double strength;
    uint32_t index;
} Onset;

static int cmp_double(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return (x > y) - (x < y);
}

// Strongest first; equal strengths keep their order (like Python's stable sort)
static int cmp_onset(const void* a, const void* b) {
    const Onset* x = a;
    const Onset* y = b;
    if (x->strength != y->strength)
        return x->strength < y->strength ? 1 : -1;
    return (x->index > y->index) - (x->index < y->index);
}

static int cmp_u32(const void* a, const void* b) {
    uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
    return (x > y) - (x < y);
}

uint32_t slicer_transients(const Envelope* envelope, uint32_t source_frames,
                           float threshold, float min_gap_ms, SliceDef* out) {
    uint32_t n = envelope->count;
    if (n < 3 || !envelope->env || !source_frames || !envelope->source_rate)
        return 0;

    double* flux = malloc(n * sizeof(double));
    double* smooth = malloc(n * sizeof(double));
    Onset* onsets = malloc(n * sizeof(Onset));
    if (!flux || !smooth || !onsets) {
        free(flux);
        free(smooth);
        free(onsets);
        return 0;
    }

    // Positive energy flux, smoothed over 3 values
    const double* env = envelope->env;
    flux[0] = 0.0;
    for (uint32_t i = 1; i < n; i++) {
        double d = env[i] - env[i - 1];
        flux[i] = d > 0.0 ? d : 0.0;
    }
    // np.convolve(flux, ones(3) / 3, "same")
    for (uint32_t i = 0; i < n; i++) {
        double third = 1.0 / 3.0;
        double sum = i + 1 < n ? flux[i + 1] * third : 0.0;
        sum += flux[i] * third;
        if (i > 0)
            sum += flux[i - 1] * third;
        smooth[i] = sum;
    }

    double max_flux = 0.0;
    for (uint32_t i = 0; i < n; i++)
        if (smooth[i] > max_flux)
            max_flux = smooth[i];
    uint32_t result = 0;
    if (max_flux > 0.0) {
        // Median
        memcpy(flux, smooth, n * sizeof(double));
        qsort(flux, n, sizeof(double), cmp_double);
        double median = n % 2 ? flux[n / 2] : 0.5 * (flux[n / 2 - 1] + flux[n / 2]);

        double t = threshold < 0.0f ? 0.0 : (threshold > 100.0f ? 100.0 : threshold);
        double onset_threshold = median + (t / 100.0) * (max_flux - median);

        // Local maxima above the threshold
        uint32_t candidates = 0;
        for (uint32_t i = 1; i + 1 < n; i++) {
            if (smooth[i] >= onset_threshold && smooth[i] >= smooth[i - 1] && smooth[i] > smooth[i + 1]) {
                onsets[candidates].strength = smooth[i];
                onsets[candidates].index = i;
                candidates++;
            }
        }
        qsort(onsets, candidates, sizeof(Onset), cmp_onset);

        // Keep the strongest onsets that respect the minimum gap
        double gap_ms = min_gap_ms < 1.0f ? 1.0 : min_gap_ms;
        uint32_t min_gap = (uint32_t)(envelope->source_rate * gap_ms / 1000.0);
        if (min_gap < 1)
            min_gap = 1;
        uint32_t selected[MAX_SLICES];
        uint32_t nsel = 0;
        for (uint32_t c = 0; c < candidates && nsel < MAX_SLICES - 1; c++) {
            uint32_t pos = onsets[c].index * envelope->hop;
            int ok = 1;
            for (uint32_t s = 0; s < nsel; s++) {
                uint32_t d = pos > selected[s] ? pos - selected[s] : selected[s] - pos;
                if (d < min_gap) {
                    ok = 0;
                    break;
                }
            }
            if (ok)
                selected[nsel++] = pos;
        }
        qsort(selected, nsel, sizeof(uint32_t), cmp_u32);

        // Contiguous slices between 0, the onsets and the end
        uint32_t prev = 0;
        for (uint32_t s = 0; s <= nsel; s++) {
            uint32_t pos = s < nsel ? selected[s] : source_frames;
            if (pos > source_frames)
                pos = source_frames;
            if (pos > prev) {
                out[result].start = prev;
                out[result].end = pos;
                out[result].mode = DEFAULT_PLAY_MODE;
                result++;
                prev = pos;
            }
        }
    }
    free(flux);
    free(smooth);
    free(onsets);
    return result;
}

uint32_t slicer_normalize(const SliceDef* in, uint32_t count, uint32_t frames,
                          int base_note, SliceDef* out) {
    if (frames == 0)
        return 0;
    uint32_t n = 0;
    int64_t last_start = -1;
    for (uint32_t i = 0; i < count && n < MAX_SLICES; i++) {
        SliceDef sl = in[i];
        if (sl.start >= sl.end)
            continue;
        int64_t start = sl.start < frames - 1 ? sl.start : frames - 1;
        if (start < last_start + 1)
            start = last_start + 1;
        if (start >= frames)
            break;
        out[n].start = (uint32_t)start;
        out[n].end = sl.end;
        out[n].mode = sl.mode < PLAY_MODE_COUNT ? sl.mode : DEFAULT_PLAY_MODE;
        last_start = start;
        n++;
    }
    uint32_t limit = (uint32_t)slicer_max_slices(base_note);
    if (n > limit)
        n = limit;
    if (n == 0)
        return 0;
    for (uint32_t i = 0; i + 1 < n; i++)
        out[i].end = out[i + 1].start;
    SliceDef* last = &out[n - 1];
    uint32_t end = last->end < frames ? last->end : frames;
    last->end = end > last->start + 1 ? end : last->start + 1;
    return n;
}

// ----------------------------------------------------------------------------
// Model
// ----------------------------------------------------------------------------

void model_init(Model* model) {
    memset(model, 0, sizeof(Model));
    model->params.slice_mode = SLICE_BEATS;
    model->params.slice_count = 8;
    model->params.beats = 4;
    model->params.subdivision = 1;
    model->params.threshold = 50.0f;
    model->params.min_gap_ms = 50.0f;
    model->params.base_note = DEFAULT_BASE_NOTE;
}

void model_free(Model* model) {
    envelope_free(&model->envelope);
}

/* Keep the selection on an existing slice. Without slices it is kept as is,
   so a selection restored before its sample loads isn't lost. */
static void clamp_selection(Model* model) {
    if (model->count)
        model->selected = clampi(model->selected, 0, (int)model->count - 1);
    else if (model->selected < 0)
        model->selected = 0;
}

/* Replace the slices, normalized for the current sample and base note. */
static void apply_slices(Model* model, const SliceDef* slices, uint32_t count) {
    model->count = slicer_normalize(slices, count, model->source_frames,
                                    model->params.base_note, model->slices);
    clamp_selection(model);
}

static void auto_slice(Model* model) {
    SliceDef slices[MAX_SLICES];
    uint32_t n;
    const SliceParams* p = &model->params;
    uint32_t frames = model->source_frames;
    if (!model->path[0] || !frames) {
        model->count = 0;
        model->manual = 0;
        clamp_selection(model);
        return;
    }
    switch (p->slice_mode) {
        case SLICE_BEATS:
            n = slicer_even(frames, clampi(p->beats, 1, 64) * clampi(p->subdivision, 1, 16), slices);
            break;
        case SLICE_TRANSIENT:
            n = slicer_transients(&model->envelope, frames, p->threshold, p->min_gap_ms, slices);
            if (n < 2) // No meaningful transients => fall back to a grid
                n = slicer_even(frames, p->slice_count, slices);
            break;
        case SLICE_MANUAL:
            if (model->count) {
                // Keep the hand-edited slices (and their edit flag)
                apply_slices(model, model->slices, model->count);
                return;
            }
            n = slicer_even(frames, p->slice_count, slices);
            break;
        default:
            n = slicer_even(frames, p->slice_count, slices);
            break;
    }
    apply_slices(model, slices, n);
    model->manual = 0;
}

static int map_matches(const Model* model, const SliceMap* map) {
    return model->path[0] && !strcmp(map->sample, model->path) && map->frames == model->source_frames;
}

static void use_map(Model* model, const SliceMap* map) {
    apply_slices(model, map->slices, map->count);
    // Maps from before manual editing existed count as edited, so changing
    // analysis settings can't silently discard them.
    model->manual = map->manual;
}

void model_set_sample(Model* model, const char* path, uint32_t frames, Envelope* envelope) {
    int same = model->count && !strcmp(model->path, path) && model->source_frames == frames;
    envelope_free(&model->envelope);
    model->envelope = *envelope;
    memset(envelope, 0, sizeof(Envelope));
    snprintf(model->path, sizeof(model->path), "%s", path);
    model->source_frames = frames;
    if (model->has_pending_map && map_matches(model, &model->pending_map)) {
        use_map(model, &model->pending_map);
        model->has_pending_map = 0;
    } else if (same) {
        apply_slices(model, model->slices, model->count);
    } else {
        auto_slice(model);
    }
}

void model_clear_sample(Model* model) {
    envelope_free(&model->envelope);
    model->path[0] = 0;
    model->source_frames = 0;
    model->count = 0;
    model->manual = 0;
    model->selected = 0;
}

void model_set_map(Model* model, const SliceMap* map) {
    if (map_matches(model, map)) {
        use_map(model, map);
        model->has_pending_map = 0;
    } else {
        model->pending_map = *map;
        model->has_pending_map = 1;
    }
}

void model_clear_pending_map(Model* model) {
    model->has_pending_map = 0;
}

void model_set_params(Model* model, const SliceParams* params) {
    SliceParams old = model->params;
    model->params = *params;
    model->params.base_note = clampi(params->base_note, 1, MAX_NOTE);
    int analysis_changed = old.slice_mode != params->slice_mode
                           || old.slice_count != params->slice_count
                           || old.beats != params->beats
                           || old.subdivision != params->subdivision
                           || old.threshold != params->threshold
                           || old.min_gap_ms != params->min_gap_ms;
    if (analysis_changed && !model->manual && params->slice_mode != SLICE_MANUAL)
        auto_slice(model);
    else if (old.base_note != model->params.base_note)
        apply_slices(model, model->slices, model->count);
}

void model_detect(Model* model) {
    auto_slice(model);
}

void model_select(Model* model, int index) {
    model->selected = index;
    clamp_selection(model);
}

void model_set_start(Model* model, uint32_t frame) {
    if (!model->count)
        return;
    int i = model->selected;
    SliceDef* sl = &model->slices[i];
    uint32_t min_start = i > 0 ? model->slices[i - 1].start + 1 : 0;
    uint32_t max_start = sl->end - 1;
    if (max_start < min_start)
        return;
    uint32_t start = frame < min_start ? min_start : (frame > max_start ? max_start : frame);
    if (start == sl->start)
        return; // No change: not a hand edit
    sl->start = start;
    if (i > 0)
        model->slices[i - 1].end = sl->start;
    model->manual = 1;
}

void model_set_end(Model* model, uint32_t frame) {
    if (!model->count)
        return;
    int i = model->selected;
    SliceDef* sl = &model->slices[i];
    uint32_t min_end = sl->start + 1;
    uint32_t max_end = i < (int)model->count - 1 ? model->slices[i + 1].end - 1 : model->source_frames;
    if (max_end < min_end)
        return;
    uint32_t end = frame < min_end ? min_end : (frame > max_end ? max_end : frame);
    if (end == sl->end)
        return; // No change: not a hand edit
    sl->end = end;
    if (i < (int)model->count - 1)
        model->slices[i + 1].start = sl->end;
    model->manual = 1;
}

void model_set_play_mode(Model* model, int mode) {
    if (!model->count || mode < 0 || mode >= PLAY_MODE_COUNT || model->slices[model->selected].mode == mode)
        return;
    model->slices[model->selected].mode = (uint8_t)mode;
    model->manual = 1;
}

void model_fill_table(const Model* model, SliceTable* table) {
    table->count = model->count;
    table->source_frames = model->source_frames;
    table->base_note = (uint8_t)model->params.base_note;
    memcpy(table->slices, model->slices, model->count * sizeof(SliceDef));
}

// ----------------------------------------------------------------------------
// Slice map files
// ----------------------------------------------------------------------------

size_t slicemap_format(const Model* model, int live, char* buf, size_t size) {
    size_t len = 0;
    int n;
#define APPEND(...) do { \
        n = snprintf(buf + len, size - len, __VA_ARGS__); \
        if (n < 0 || (size_t)n >= size - len) return 0; \
        len += n; \
    } while (0)
    APPEND("# Zynthian Slicer slice map\nversion=1\n");
    APPEND("sample=%s\nframes=%u\nmanual=%d\n", model->path, model->source_frames, model->manual);
    if (live) {
        APPEND("selected=%d\nbase_note=%d\n", model->selected, model->params.base_note);
        if (model->last_export[0])
            APPEND("export=%s\n", model->last_export);
        if (model->last_export_error[0])
            APPEND("export_error=%s\n", model->last_export_error);
    }
    for (uint32_t i = 0; i < model->count; i++) {
        const SliceDef* sl = &model->slices[i];
        APPEND("slice=%u %u %s\n", sl->start, sl->end, play_mode_name(sl->mode));
    }
#undef APPEND
    return len;
}

int slicemap_parse(const char* text, SliceMap* map) {
    memset(map, 0, sizeof(SliceMap));
    int version = 0;
    const char* line = text;
    while (line && *line) {
        const char* eol = strchr(line, '\n');
        size_t len = eol ? (size_t)(eol - line) : strlen(line);
        char buf[SLICER_PATH_MAX + 32];
        if (len >= sizeof(buf))
            len = sizeof(buf) - 1;
        memcpy(buf, line, len);
        buf[len] = 0;
        if (len && buf[len - 1] == '\r')
            buf[len - 1] = 0;
        line = eol ? eol + 1 : NULL;

        char* eq = strchr(buf, '=');
        if (buf[0] == '#' || !eq)
            continue;
        *eq = 0;
        const char* key = buf;
        const char* value = eq + 1;
        if (!strcmp(key, "version")) {
            version = atoi(value);
        } else if (!strcmp(key, "sample")) {
            snprintf(map->sample, sizeof(map->sample), "%s", value);
        } else if (!strcmp(key, "frames")) {
            map->frames = (uint32_t)strtoul(value, NULL, 10);
        } else if (!strcmp(key, "manual")) {
            map->manual = atoi(value) != 0;
        } else if (!strcmp(key, "selected")) {
            map->selected = atoi(value);
        } else if (!strcmp(key, "slice") && map->count < MAX_SLICES) {
            unsigned long start, end;
            char mode[16] = "";
            if (sscanf(value, "%lu %lu %15s", &start, &end, mode) >= 2) {
                SliceDef* sl = &map->slices[map->count++];
                sl->start = (uint32_t)start;
                sl->end = (uint32_t)end;
                sl->mode = DEFAULT_PLAY_MODE;
                for (int m = 0; m < PLAY_MODE_COUNT; m++)
                    if (!strcmp(mode, PLAY_MODE_NAMES[m]))
                        sl->mode = (uint8_t)m;
            }
        }
    }
    return version == 1 && map->sample[0] && map->frames ? 0 : -1;
}

int slicemap_load(const char* path, SliceMap* map) {
    FILE* f = fopen(path, "r");
    if (!f)
        return -1;
    size_t size = 16384 + MAX_SLICES * 40;
    char* text = malloc(size);
    if (!text) {
        fclose(f);
        return -1;
    }
    size_t len = fread(text, 1, size - 1, f);
    fclose(f);
    text[len] = 0;
    int result = slicemap_parse(text, map);
    free(text);
    return result;
}

int write_file_atomic(const char* path, const char* text, size_t len) {
    char tmp[SLICER_PATH_MAX + 16];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp))
        return -1;
    FILE* f = fopen(tmp, "w");
    if (!f)
        return -1;
    int ok = fwrite(text, 1, len, f) == len;
    ok = (fclose(f) == 0) && ok;
    if (!ok || rename(tmp, path)) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

uint64_t slicer_hash(const char* text, size_t len) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= (unsigned char)text[i];
        h *= 1099511628211ULL;
    }
    return h;
}
