/*
 * ******************************************************************
 * Zynthian Slicer LV2 plugin: SFZ export (worker thread)
 *
 * Writes a self-contained SFZ instrument (one region per slice) plus a WAV
 * copy of the sample, laid out as a sfizz bank: <output_dir>/<name>/<name>.sfz
 * A <name>.yml next to it gives the Zynthian sfizz chain a "tune" control.
 * Ported from zynthian_slicer_sfz.py.
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

#ifndef SLICER_EXPORT_H
#define SLICER_EXPORT_H

#include <stddef.h>

#include "common.h"

typedef struct {
    float gain_db;          // Master gain applied to every region
    int tune;               // Initial tuning in semitones (adjustable in sfizz with a CC)
    int polyphony;          // Maximum simultaneous voices
    double fade_seconds;    // Attack/release matching the slicer's voice fades
    int base_note;          // MIDI note of the first slice
} SfzOptions;

/** @brief  Build the SFZ text
    @retval Length of the text, or 0 on error
*/
size_t sfz_build(const SliceDef* slices, uint32_t count, const char* sample_name,
                 const SfzOptions* options, char* buf, size_t size);

/** @brief  Export the slices as an SFZ instrument
    @param  out_path Receives the path of the .sfz file
    @param  error Receives an error message on failure
    @retval 0 on success
*/
int sfz_export(const char* sample_path, const SliceDef* slices, uint32_t count,
               const char* output_dir, const SfzOptions* options,
               char* out_path, size_t out_size, char* error, size_t error_size);

#endif
