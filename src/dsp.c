/*
 * ******************************************************************
 * Zynthian Slicer LV2 plugin: voices (audio thread only)
 *
 * Based on the zynslicer library voice engine.
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

#include "dsp.h"

#include <string.h>

void dsp_reset(VoicePool* pool) {
    pool->voice_order = 0;
    memset(pool->voices, 0, sizeof(pool->voices));
}

void dsp_set_polyphony(VoicePool* pool, int voices) {
    if (voices < 1)
        voices = 1;
    if (voices > MAX_VOICES)
        voices = MAX_VOICES;
    if (pool->polyphony == voices)
        return;
    pool->polyphony = (uint8_t)voices;
    for (int i = voices; i < VOICE_SLOTS; i++)
        pool->voices[i].playing = 0;
}

/* Get a free voice, stealing the one furthest through its slice if needed.
   Only the first "polyphony" slots take new voices. A stolen voice is moved
   to a spare slot to fade out instead of being cut (a click). */
static Voice* get_free_voice(VoicePool* pool) {
    uint8_t limit = pool->polyphony;
    if (limit > MAX_VOICES)
        limit = MAX_VOICES;
    if (limit == 0)
        limit = 1;
    for (uint8_t i = 0; i < limit; i++) {
        if (!pool->voices[i].playing)
            return &pool->voices[i];
    }
    Voice* victim = &pool->voices[0];
    for (uint8_t i = 1; i < limit; i++) {
        if (pool->voices[i].pos > victim->pos)
            victim = &pool->voices[i];
    }
    for (uint8_t i = limit; i < VOICE_SLOTS; i++) {
        Voice* spare = &pool->voices[i];
        if (!spare->playing) {
            *spare = *victim;
            if (!spare->fade_out)
                spare->fade_out = FADE_FRAMES;
            break;
        }
    }
    return victim;
}

int dsp_note_on(VoicePool* pool, const SliceTable* table, uint8_t note, uint8_t velocity, uint32_t offset) {
    if (!table || note < table->base_note)
        return 0;
    uint32_t index = note - table->base_note;
    if (index >= table->count)
        return 0;
    Voice* voice = get_free_voice(pool);
    memset(voice, 0, sizeof(Voice));
    voice->playing = 1;
    voice->note = note;
    voice->velocity = velocity ? velocity : 127;
    voice->slice = (uint8_t)index;
    voice->start_frame = offset;
    voice->order = ++pool->voice_order;
    voice->fade_in = FADE_FRAMES;
    return 1;
}

void dsp_note_off(VoicePool* pool, const SliceTable* table, uint8_t note) {
    for (uint8_t i = 0; i < VOICE_SLOTS; i++) {
        Voice* voice = &pool->voices[i];
        if (!voice->playing || voice->fade_out)
            continue;
        if (note && voice->note != note)
            continue;
        uint8_t mode = PLAY_ONE_SHOT;
        if (table && voice->slice < table->count)
            mode = table->slices[voice->slice].mode;
        if (note == 0 || mode == PLAY_GATE || mode == PLAY_LOOP)
            voice->fade_out = FADE_FRAMES;
    }
}

/* Get a slice's range in sample frames (host rate). Returns 0 if empty. */
static int slice_range(const SliceTable* table, const Sample* sample, uint8_t index,
                       uint32_t* start, uint32_t* end) {
    if (!table || !sample || index >= table->count || !sample->source_frames)
        return 0;
    const SliceDef* sl = &table->slices[index];
    uint64_t s = (uint64_t)sl->start * sample->frames / sample->source_frames;
    uint64_t e = (uint64_t)sl->end * sample->frames / sample->source_frames;
    if (e > sample->frames)
        e = sample->frames;
    if (s >= e)
        return 0;
    *start = (uint32_t)s;
    *end = (uint32_t)e;
    return 1;
}

static void process_voice(Voice* voice, const SliceTable* table, const Sample* sample,
                          float gain, double rate, float* out_l, float* out_r, uint32_t frames) {
    uint32_t start, end;
    if (!slice_range(table, sample, voice->slice, &start, &end)) {
        voice->playing = 0;
        return;
    }
    uint32_t length = end - start;
    int loop = table->slices[voice->slice].mode == PLAY_LOOP;

    if (voice->pos >= length) {
        if (!loop) {
            voice->playing = 0;
            return;
        }
        voice->pos = 0; // Slice was shortened while playing
    }

    // Note-on offset within this cycle
    uint32_t offset = 0;
    if (voice->start_frame) {
        if (voice->start_frame >= frames) {
            voice->start_frame -= frames;
            return;
        }
        offset = voice->start_frame;
        voice->start_frame = 0;
    }
    uint32_t n = frames - offset;

    // Looping slices fade at both edges so the jump back to the start doesn't click
    uint32_t loop_fade = 0;
    if (loop) {
        loop_fade = length / 4;
        if (loop_fade > FADE_FRAMES)
            loop_fade = FADE_FRAMES;
    }

    // Tuned voices step through the sample at a fractional rate with linear
    // interpolation. At rate 1.0 the position stays whole, so the output is
    // the sample data unchanged.
    if (!(rate > 0.0))
        rate = 1.0;
    const float* data_l = sample->data[0] + start;
    const float* data_r = sample->data[1] + start;
    float vgain = gain * (voice->velocity / 127.0f);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t idx = (uint32_t)voice->pos;
        float frac = (float)(voice->pos - idx);
        float g = vgain;
        if (loop_fade) {
            double edge = voice->pos < length - 1 - voice->pos ? voice->pos : length - 1 - voice->pos;
            if (edge < 0.0)
                edge = 0.0;
            if (edge < loop_fade)
                g *= (float)(edge + 1) / (loop_fade + 1);
        }
        if (voice->fade_in) {
            g *= 1.0f - (float)voice->fade_in / FADE_FRAMES;
            voice->fade_in--;
        }
        if (voice->fade_out) {
            g *= (float)voice->fade_out / FADE_FRAMES;
            voice->fade_out--;
            if (!voice->fade_out)
                voice->playing = 0;
        }
        float a = data_l[idx];
        float b = data_r[idx];
        if (frac > 0.0f) {
            // The frame after the last one is the slice start when looping
            uint32_t next = idx + 1 < length ? idx + 1 : (loop ? 0 : idx);
            a += (data_l[next] - a) * frac;
            b += (data_r[next] - b) * frac;
        }
        out_l[offset + i] += a * g;
        out_r[offset + i] += b * g;
        voice->pos += rate;
        if (voice->pos >= length) {
            if (loop) {
                voice->pos -= length;
                if (voice->pos >= length)
                    voice->pos = 0; // Rate larger than the slice
            } else {
                voice->playing = 0;
            }
        }
        if (!voice->playing)
            break;
    }
}

void dsp_process(VoicePool* pool, const SliceTable* table, const Sample* sample,
                 float gain, double rate, float* out_l, float* out_r, uint32_t frames) {
    for (uint8_t i = 0; i < VOICE_SLOTS; i++) {
        Voice* voice = &pool->voices[i];
        if (!voice->playing)
            continue;
        if (!table || !sample) {
            voice->playing = 0;
            continue;
        }
        process_voice(voice, table, sample, gain, rate, out_l, out_r, frames);
    }
}

int64_t dsp_playheads(const VoicePool* pool, const SliceTable* table, const Sample* sample,
                      uint32_t active[(MAX_SLICES + 31) / 32]) {
    memset(active, 0, sizeof(uint32_t) * ((MAX_SLICES + 31) / 32));
    int64_t latest_pos = -1;
    uint32_t latest_order = 0;
    for (uint8_t i = 0; i < VOICE_SLOTS; i++) {
        const Voice* voice = &pool->voices[i];
        uint32_t start, end;
        if (!voice->playing || !slice_range(table, sample, voice->slice, &start, &end))
            continue;
        active[voice->slice / 32] |= 1u << (voice->slice % 32);
        if (voice->order > latest_order) {
            latest_order = voice->order;
            const SliceDef* sl = &table->slices[voice->slice];
            double progress = voice->pos / (double)(end - start);
            if (progress > 1.0)
                progress = 1.0;
            latest_pos = sl->start + (int64_t)(progress * (sl->end - sl->start));
        }
    }
    return latest_pos;
}
