/*
 * ******************************************************************
 * Zynthian Slicer LV2 plugin: shared definitions
 *
 * ******************************************************************
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * For a full copy of the GNU General Public License see the LICENSE.txt file.
 *
 * ******************************************************************
 */

#ifndef SLICER_COMMON_H
#define SLICER_COMMON_H

#include <stdint.h>

#define SLICER_URI "https://github.com/xjaner/zynthian-slicer"

#define MAX_SLICES 127      // One slice per MIDI note 1..127
#define MAX_NOTE 127
#define MAX_VOICES 16       // Maximum simultaneous voices
#define VOICE_SLOTS (MAX_VOICES * 2) // Voice pool incl. spare slots for stolen voices to fade out
#define FADE_FRAMES 64      // Voice fade-in / fade-out in frames
#define MIN_FRAMES 4096     // Shortest audio file accepted, in frames
#define MAX_TUNE 24         // Tune range in semitones, up or down
#define DEFAULT_BASE_NOTE 36 // C2
#define SLICER_PATH_MAX 1024

// How a slice plays: to its end, while the key is held, or repeating while held
enum PLAY_MODE {
    PLAY_ONE_SHOT = 0,
    PLAY_GATE = 1,
    PLAY_LOOP = 2,
    PLAY_MODE_COUNT
};
#define DEFAULT_PLAY_MODE PLAY_GATE

// How slices are calculated from the sample
enum SLICE_MODE {
    SLICE_BEATS = 0,
    SLICE_GRID = 1,
    SLICE_TRANSIENT = 2,
    SLICE_MANUAL = 3,
    SLICE_MODE_COUNT
};

typedef struct {
    uint32_t start;     // First frame, in source file frames
    uint32_t end;       // Frame after the last one, in source file frames
    uint8_t mode;       // PLAY_MODE
} SliceDef;

/* Immutable slice list handed from the worker to the audio thread.
   Slice i plays on MIDI note base_note + i. */
typedef struct {
    uint32_t count;
    uint32_t source_frames; // Sample length in source file frames
    uint8_t base_note;
    SliceDef slices[MAX_SLICES];
} SliceTable;

/* Sample audio, resampled to the host rate, handed from the worker to the
   audio thread. */
typedef struct {
    uint32_t frames;        // Frames at the host samplerate
    uint32_t source_frames; // Frames in the source file
    uint8_t channels;       // 1 or 2 (data[1] == data[0] for mono)
    float* data[2];
} Sample;

#endif
