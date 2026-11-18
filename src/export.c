/*
 * ******************************************************************
 * Zynthian Slicer LV2 plugin: SFZ export (worker thread)
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

#include "export.h"
#include "model.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sndfile.h>

// Records which sample an export folder was made from, so re-exporting the
// same sample overwrites it while a different sample with the same name gets
// a new folder.
#define SOURCE_MARKER ".zynthian_slicer_source"

#define MIN_VOLUME_DB -144.0
#define MAX_VOLUME_DB 6.0

// Tune is exported as a MIDI CC so it stays adjustable (and MIDI-learnable) in
// sfizz. Each CC step is one semitone and CC 64 is no change, so the range is
// -64..+63 semitones.
#define TUNE_CC 20
#define TUNE_CC_CENTER 64
#define MIN_SFZ_TUNE (-TUNE_CC_CENTER)
#define MAX_SFZ_TUNE (127 - TUNE_CC_CENTER)

static const char* SFZ_LOOP_MODES[PLAY_MODE_COUNT] = {"one_shot", "no_loop", "loop_continuous"};

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

size_t sfz_build(const SliceDef* slices, uint32_t count, const char* sample_name,
                 const SfzOptions* o, char* buf, size_t size) {
    if (!count)
        return 0;
    size_t len = 0;
    int n;
#define APPEND(...) do { \
        n = snprintf(buf + len, size - len, __VA_ARGS__); \
        if (n < 0 || (size_t)n >= size - len) return 0; \
        len += n; \
    } while (0)
    // No <control> default_path: sfizz prepends it verbatim to each sample
    // name, and samples already resolve relative to the .sfz file.
    int tune_cc_value = TUNE_CC_CENTER + clampi(o->tune, MIN_SFZ_TUNE, MAX_SFZ_TUNE);
    double volume = o->gain_db;
    if (volume < MIN_VOLUME_DB)
        volume = MIN_VOLUME_DB;
    if (volume > MAX_VOLUME_DB)
        volume = MAX_VOLUME_DB;
    APPEND("// Zynthian slicer export\n<control>\nlabel_cc%d=Tune\nset_cc%d=%d\n\n",
           TUNE_CC, TUNE_CC, tune_cc_value);
    APPEND("<global>\npolyphony=%d\nampeg_attack=%.4f\nampeg_release=%.4f\n",
           o->polyphony > 1 ? o->polyphony : 1, o->fade_seconds, o->fade_seconds);
    // Tune CC: 100 cents per CC step, centered on TUNE_CC_CENTER. Transposing
    // pitch resamples like the slicer: pitch and speed together.
    APPEND("transpose=%d\npitch_oncc%d=%d\n\n", -TUNE_CC_CENTER, TUNE_CC, 127 * 100);
    for (uint32_t i = 0; i < count; i++) {
        const SliceDef* sl = &slices[i];
        int note = o->base_note + (int)i;
        int mode = sl->mode < PLAY_MODE_COUNT ? sl->mode : DEFAULT_PLAY_MODE;
        APPEND("// slice %u\n<region>\nsample=%s\noffset=%u\nend=%u\n", i, sample_name, sl->start, sl->end - 1);
        APPEND("lokey=%d\nhikey=%d\npitch_keycenter=%d\nloop_mode=%s\n", note, note, note, SFZ_LOOP_MODES[mode]);
        if (mode == PLAY_LOOP)
            APPEND("loop_start=%u\nloop_end=%u\n", sl->start, sl->end - 1);
        APPEND("volume=%.2f\n\n", volume);
    }
#undef APPEND
    // Python's "\n".join() has no trailing newline after the last blank line
    if (len >= 1)
        buf[--len] = 0;
    return len;
}

static void build_controller_yaml(int tune, char* buf, size_t size) {
    snprintf(buf, size,
             "# Zynthian controls for this slicer export (read by the sfizz engine)\n"
             "controllers:\n"
             "  slicer:\n"
             "    tune:\n"
             "      midi_cc: %d\n"
             "      value_min: %d\n"
             "      value_max: %d\n"
             "      value: %d\n"
             "      is_integer: true\n",
             TUNE_CC, MIN_SFZ_TUNE, MAX_SFZ_TUNE, clampi(tune, MIN_SFZ_TUNE, MAX_SFZ_TUNE));
}

/* Sample file name without folder or extension, made safe for a folder name. */
static void export_base_name(const char* sample_path, char* out, size_t size) {
    const char* base = strrchr(sample_path, '/');
    base = base ? base + 1 : sample_path;
    const char* dot = strrchr(base, '.');
    size_t len = dot && dot != base ? (size_t)(dot - base) : strlen(base);
    // Replace each run of unsafe characters with one '_'
    char tmp[256];
    size_t t = 0;
    int in_bad = 0;
    for (size_t i = 0; i < len && t < sizeof(tmp) - 1; i++) {
        unsigned char c = (unsigned char)base[i];
        int ok = isalnum(c) || c == '.' || c == '_' || c == ' ' || c == '-';
        if (ok) {
            tmp[t++] = (char)c;
            in_bad = 0;
        } else if (!in_bad) {
            tmp[t++] = '_';
            in_bad = 1;
        }
    }
    tmp[t] = 0;
    // Strip " ._" from both ends
    char* s = tmp;
    while (*s && strchr(" ._", *s))
        s++;
    size_t l = strlen(s);
    while (l && strchr(" ._", s[l - 1]))
        s[--l] = 0;
    snprintf(out, size, "%s", *s ? s : "slicer");
}

static int path_exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static int make_dirs(const char* path) {
    char tmp[SLICER_PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, 0755) && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) && errno != EEXIST)
        return -1;
    return 0;
}

/* Resolve a path (as is if it doesn't exist yet). Returns -1 if it doesn't fit. */
static int absolute_path(const char* path, char* out, size_t size) {
    char* abs = realpath(path, NULL);
    int n = snprintf(out, size, "%s", abs ? abs : path);
    free(abs);
    return n < 0 || (size_t)n >= size ? -1 : 0;
}

/* Folder name: reuse this sample's folder, else find a free one. */
static void get_export_name(const char* output_dir, const char* base, const char* sample_path,
                            char* out, size_t size) {
    char src[SLICER_PATH_MAX];
    if (absolute_path(sample_path, src, sizeof(src)))
        src[0] = 0; // Too long to compare: always use a new folder
    snprintf(out, size, "%s", base);
    for (int n = 1;; n++) {
        char dir[SLICER_PATH_MAX];
        snprintf(dir, sizeof(dir), "%s/%s", output_dir, out);
        if (!path_exists(dir))
            return;
        char marker[SLICER_PATH_MAX + 32];
        snprintf(marker, sizeof(marker), "%s/%s", dir, SOURCE_MARKER);
        FILE* f = fopen(marker, "r");
        if (f) {
            char line[SLICER_PATH_MAX] = "";
            size_t len = fread(line, 1, sizeof(line) - 1, f);
            fclose(f);
            line[len] = 0;
            while (len && (line[len - 1] == '\n' || line[len - 1] == ' '))
                line[--len] = 0;
            if (!strcmp(line, src))
                return;
        }
        snprintf(out, size, "%s_%d", base, n + 1);
    }
}

static int copy_file(const char* src, const char* dst) {
    FILE* in = fopen(src, "rb");
    if (!in)
        return -1;
    FILE* out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return -1;
    }
    char buf[65536];
    size_t n;
    int ok = 1;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        if (fwrite(buf, 1, n, out) != n) {
            ok = 0;
            break;
        }
    ok = !ferror(in) && ok;
    fclose(in);
    ok = (fclose(out) == 0) && ok;
    return ok ? 0 : -1;
}

/* Copy a WAV sample, or decode any other format to 24-bit WAV. libsndfile is
   the decoder the slicer loads samples with, so slice positions stay valid. */
static int write_wav(const char* src, const char* dst) {
    const char* ext = strrchr(src, '.');
    if (ext && !strcasecmp(ext, ".wav")) {
        char a[SLICER_PATH_MAX], b[SLICER_PATH_MAX];
        if (absolute_path(src, a, sizeof(a)) || absolute_path(dst, b, sizeof(b)))
            return -1;
        if (!strcmp(a, b))
            return 0;
        return copy_file(src, dst);
    }
    SF_INFO info;
    memset(&info, 0, sizeof(info));
    SNDFILE* in = sf_open(src, SFM_READ, &info);
    if (!in)
        return -1;
    SF_INFO out_info = info;
    out_info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_24;
    SNDFILE* out = sf_open(dst, SFM_WRITE, &out_info);
    if (!out) {
        sf_close(in);
        return -1;
    }
    float* buf = malloc(4096 * sizeof(float) * info.channels);
    int ok = buf != NULL;
    sf_count_t n;
    while (ok && (n = sf_readf_float(in, buf, 4096)) > 0)
        ok = sf_writef_float(out, buf, n) == n;
    free(buf);
    sf_close(in);
    ok = (sf_close(out) == 0) && ok;
    return ok ? 0 : -1;
}

int sfz_export(const char* sample_path, const SliceDef* slices, uint32_t count,
               const char* output_dir, const SfzOptions* options,
               char* out_path, size_t out_size, char* error, size_t error_size) {
    out_path[0] = 0;
    error[0] = 0;
    if (!count || !sample_path[0]) {
        snprintf(error, error_size, "no slices");
        return -1;
    }
    char base[256], name[300];
    export_base_name(sample_path, base, sizeof(base));
    get_export_name(output_dir, base, sample_path, name, sizeof(name));

    char dir[SLICER_PATH_MAX], wav[SLICER_PATH_MAX + 320], sfz[SLICER_PATH_MAX + 320];
    char yml[SLICER_PATH_MAX + 320], marker[SLICER_PATH_MAX + 320], wav_name[320];
    int n = snprintf(dir, sizeof(dir), "%s/%s", output_dir, name);
    if (n < 0 || (size_t)n >= sizeof(dir)) {
        snprintf(error, error_size, "path too long");
        return -1;
    }
    snprintf(wav_name, sizeof(wav_name), "%s.wav", name);
    snprintf(wav, sizeof(wav), "%s/%s", dir, wav_name);
    snprintf(sfz, sizeof(sfz), "%s/%s.sfz", dir, name);
    snprintf(yml, sizeof(yml), "%s/%s.yml", dir, name);
    snprintf(marker, sizeof(marker), "%s/%s", dir, SOURCE_MARKER);

    if (make_dirs(dir)) {
        snprintf(error, error_size, "can't create folder");
        return -1;
    }
    if (write_wav(sample_path, wav)) {
        snprintf(error, error_size, "can't write WAV");
        return -1;
    }
    size_t size = 4096 + count * 256;
    char* text = malloc(size);
    if (!text) {
        snprintf(error, error_size, "out of memory");
        return -1;
    }
    size_t len = sfz_build(slices, count, wav_name, options, text, size);
    int ok = len > 0 && write_file_atomic(sfz, text, len) == 0;
    if (ok) {
        build_controller_yaml(options->tune, text, size);
        ok = write_file_atomic(yml, text, strlen(text)) == 0;
    }
    if (ok) {
        ok = absolute_path(sample_path, text, size) == 0
             && write_file_atomic(marker, text, strlen(text)) == 0;
    }
    free(text);
    if (!ok) {
        snprintf(error, error_size, "can't write SFZ");
        return -1;
    }
    snprintf(out_path, out_size, "%s", sfz);
    return 0;
}
