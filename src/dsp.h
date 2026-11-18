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

#ifndef SLICER_DSP_H
#define SLICER_DSP_H

#include "common.h"

typedef struct {
    uint8_t playing;        // 1 while the voice sounds
    uint8_t note;           // MIDI note that triggered the voice
    uint8_t velocity;       // MIDI velocity
    uint8_t slice;          // Index of the slice in the slice table
    double pos;             // Position within the slice in sample frames (fractional when tuned)
    uint32_t start_frame;   // Note-on offset within the current cycle
    uint32_t order;         // Voice start order (higher is newer)
    uint32_t fade_in;       // Remaining fade-in frames
    uint32_t fade_out;      // Remaining fade-out frames
} Voice;

typedef struct {
    Voice voices[VOICE_SLOTS];
    uint8_t polyphony;      // Voices available to new notes
    uint32_t voice_order;   // Monotonic voice start counter
} VoicePool;

/** @brief  Silence all voices immediately */
void dsp_reset(VoicePool* pool);

/** @brief  Set the polyphony (1..MAX_VOICES), dropping voices above it */
void dsp_set_polyphony(VoicePool* pool, int voices);

/** @brief  Start the slice mapped to a note
    @param  offset Frame offset within the current cycle
    @retval 1 if a voice started, else 0
*/
int dsp_note_on(VoicePool* pool, const SliceTable* table, uint8_t note, uint8_t velocity, uint32_t offset);

/** @brief  Release a note as a note-off does (gate and loop slices fade out)
    @param  note MIDI note, or 0 to fade out all voices
*/
void dsp_note_off(VoicePool* pool, const SliceTable* table, uint8_t note);

/** @brief  Mix all voices into the output buffers (which are not cleared)
    @param  gain Linear master gain
    @param  rate Playback rate (1.0 = original pitch and speed)
*/
void dsp_process(VoicePool* pool, const SliceTable* table, const Sample* sample,
                 float gain, double rate, float* out_l, float* out_r, uint32_t frames);

/** @brief  Get the playing slices and the newest voice's position
    @param  active Receives one bit per slice index (MAX_SLICES bits)
    @retval Position of the newest voice in source frames, or -1 if none
*/
int64_t dsp_playheads(const VoicePool* pool, const SliceTable* table, const Sample* sample,
                      uint32_t active[(MAX_SLICES + 31) / 32]);

#endif
