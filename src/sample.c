/*
 * ******************************************************************
 * Zynthian Slicer LV2 plugin: sample loading (worker thread)
 *
 * Based on the zynslicer / zynclippy sample loading code.
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

#include "sample.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sndfile.h>
#include <samplerate.h>

// Good quality while staying fast enough for long samples on a Raspberry Pi
#define SRC_QUALITY SRC_SINC_FASTEST

void sample_free(Sample* sample) {
    if (!sample)
        return;
    free(sample->data[0]);
    if (sample->data[1] != sample->data[0])
        free(sample->data[1]);
    free(sample);
}

void envelope_free(Envelope* envelope) {
    if (!envelope)
        return;
    free(envelope->env);
    memset(envelope, 0, sizeof(Envelope));
}

/* RMS envelope of the mono mix in blocks of 10 ms (at least 64 frames). */
static int compute_envelope(const float* interleaved, uint32_t frames, int channels,
                            uint32_t rate, Envelope* envelope) {
    uint32_t hop = (uint32_t)(rate * 0.01);
    if (hop < 64)
        hop = 64;
    envelope->hop = hop;
    envelope->source_rate = rate;
    envelope->count = 0;
    envelope->env = NULL;
    if (frames <= hop)
        return 0;
    uint32_t count = (frames - hop) / hop + 1;
    envelope->env = malloc(count * sizeof(double));
    if (!envelope->env)
        return -1;
    // Block energies as differences of a running sum, like numpy's cumsum,
    // so detection matches the Python slicer to the last bit
    double cum = 0.0;
    double block_start = 0.0;
    for (uint32_t i = 0; i < count; i++) {
        const float* p = interleaved + (size_t)i * hop * channels;
        for (uint32_t f = 0; f < hop; f++) {
            // Mono mix in single precision, like numpy's mean of float32 data
            float mono = 0.0f;
            for (int ch = 0; ch < channels; ch++)
                mono += p[f * channels + ch];
            mono /= channels;
            float energy = mono * mono;
            cum += energy;
        }
        envelope->env[i] = sqrt((cum - block_start) / hop + 1e-12);
        block_start = cum;
    }
    envelope->count = count;
    return 0;
}

int sample_load(const char* path, double host_rate, Sample** out_sample, Envelope* envelope) {
    *out_sample = NULL;
    memset(envelope, 0, sizeof(Envelope));

    SF_INFO info;
    memset(&info, 0, sizeof(info));
    SNDFILE* snd = sf_open(path, SFM_READ, &info);
    if (!snd || info.samplerate < 11000 || info.channels < 1 || info.frames < MIN_FRAMES
            || info.frames > 0x7fffffff / info.channels) {
        fprintf(stderr, "slicer: Wrong sample file '%s'\n", path);
        if (snd)
            sf_close(snd);
        return -1;
    }
    int channels = info.channels;
    uint32_t frames = (uint32_t)info.frames;
    float* data_in = malloc((size_t)frames * channels * sizeof(float));
    if (!data_in) {
        sf_close(snd);
        return -1;
    }
    sf_count_t count = sf_readf_float(snd, data_in, frames);
    sf_close(snd);
    if (count < MIN_FRAMES) {
        fprintf(stderr, "slicer: Can't read sample data from '%s'\n", path);
        free(data_in);
        return -1;
    }
    // Some formats report more frames than they decode: trust what was read
    frames = (uint32_t)count;

    if (compute_envelope(data_in, frames, channels, info.samplerate, envelope)) {
        free(data_in);
        return -1;
    }

    Sample* sample = calloc(1, sizeof(Sample));
    if (!sample) {
        free(data_in);
        envelope_free(envelope);
        return -1;
    }
    sample->source_frames = frames;
    sample->channels = channels < 2 ? 1 : 2;

    // Deinterleave up to two channels
    for (int ch = 0; ch < sample->channels; ch++) {
        sample->data[ch] = malloc((size_t)frames * sizeof(float));
        if (!sample->data[ch])
            goto fail;
        for (uint32_t i = 0; i < frames; i++)
            sample->data[ch][i] = data_in[(size_t)i * channels + ch];
    }
    free(data_in);
    data_in = NULL;
    sample->frames = frames;

    // Resample each channel to the host samplerate
    if ((int)host_rate != info.samplerate) {
        double ratio = host_rate / info.samplerate;
        long max_out = (long)(frames * ratio) + 1;
        long generated = 0;
        for (int ch = 0; ch < sample->channels; ch++) {
            float* resampled = malloc(max_out * sizeof(float));
            if (!resampled)
                goto fail;
            SRC_DATA src = {
                .data_in = sample->data[ch],
                .data_out = resampled,
                .input_frames = frames,
                .output_frames = max_out,
                .src_ratio = ratio,
                .end_of_input = 1
            };
            int err = src_simple(&src, SRC_QUALITY, 1);
            if (err) {
                fprintf(stderr, "slicer: Resampling error: %s\n", src_strerror(err));
                free(resampled);
                goto fail;
            }
            free(sample->data[ch]);
            sample->data[ch] = resampled;
            generated = src.output_frames_gen;
        }
        if (generated <= 0)
            goto fail;
        sample->frames = (uint32_t)generated;
    }
    if (sample->channels == 1)
        sample->data[1] = sample->data[0];

    *out_sample = sample;
    return 0;

fail:
    free(data_in);
    if (sample->channels == 1)
        sample->data[1] = NULL;
    sample_free(sample);
    envelope_free(envelope);
    return -1;
}
